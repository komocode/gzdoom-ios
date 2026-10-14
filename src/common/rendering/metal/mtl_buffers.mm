/*
** mtl_buffers.mm
** Vertex, index and data buffers for the Metal backend.
**
** All buffers live in shared memory. The GPU may still execute earlier frames
** while the CPU records the next one (r_framesinflight), so a CPU write must
** never land in memory that an unfinished command buffer reads:
**
** - Per-frame data (stream uniforms, matrices, viewpoints, lights, bones) is
**   rewritten every frame and has one buffer per frame in flight (FrameRing).
** - The flat vertex buffer is N-buffered by FFlatVertexBuffer itself.
** - Other in-place writes (SetSubData, Lock) go to a copy while the GPU is busy.
**
** Replacing a buffer just drops our reference: command buffers keep the old one
** alive while they use it.
*/

#include "mtl_renderdevice.h"
#include "mtl_renderstate.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "v_video.h"

MtlHardwareBuffer::MtlHardwareBuffer(MetalRenderDevice *fb) : fb(fb)
{
	mRingSlot = fb->FrameSlot();
	fb->GetBufferManager()->AddBuffer(this);
}

MtlHardwareBuffer::~MtlHardwareBuffer()
{
	if (fb)
		fb->GetBufferManager()->RemoveBuffer(this);
}

void MtlHardwareBuffer::Reset()
{
	mBuffer = nil;
	for (auto &buffer : mRing)
		buffer = nil;
	map = nullptr;
}

void MtlHardwareBuffer::Replace(id<MTLBuffer> buffer)
{
	mBuffer = buffer;
	if (FrameRing)
	{
		// The other frames get a buffer of the new size when they come up.
		for (auto &slot : mRing)
			slot = nil;
		mRing[mRingSlot] = buffer;
	}
}

void MtlHardwareBuffer::Allocate(size_t size, const void *data)
{
	size_t bufsize = std::max(size, (size_t)16); // For supporting zero byte buffers
	if (data)
		Replace([fb->device newBufferWithBytes:data length:bufsize options:MTLResourceStorageModeShared]);
	else
		Replace([fb->device newBufferWithLength:bufsize options:MTLResourceStorageModeShared]);
	buffersize = size;
}

void MtlHardwareBuffer::SetFrameSlot(int slot)
{
	if (!FrameRing || !mBuffer || slot == mRingSlot)
		return;
	mRingSlot = slot;
	if (!mRing[slot] || mRing[slot].length != mBuffer.length)
		mRing[slot] = [fb->device newBufferWithLength:mBuffer.length options:MTLResourceStorageModeShared];
	mBuffer = mRing[slot];
	if (map)
		map = mBuffer.contents;
}

void MtlHardwareBuffer::SetData(size_t size, const void *data, BufferUsageType usage)
{
	if (data && size < 16)
	{
		// newBufferWithBytes reads the full allocation size
		uint8_t padded[16] = {};
		memcpy(padded, data, size);
		Allocate(size, padded);
	}
	else
	{
		Allocate(size, data);
	}
	map = (usage == BufferUsageType::Persistent) ? mBuffer.contents : nullptr;
}

void MtlHardwareBuffer::SetSubData(size_t offset, size_t size, const void *data)
{
	if (mBuffer && offset + size <= mBuffer.length)
	{
		// Partial updates (the shadow map's dynamic AABB nodes) go to a copy
		// while earlier frames may still read the buffer.
		if (!FrameRing && fb->GpuBusy())
		{
			Replace([fb->device newBufferWithBytes:mBuffer.contents length:mBuffer.length options:MTLResourceStorageModeShared]);
			if (map)
				map = mBuffer.contents;
		}
		memcpy((uint8_t*)mBuffer.contents + offset, data, size);
	}
}

void MtlHardwareBuffer::Resize(size_t newsize)
{
	newsize = std::max(newsize, (size_t)16);
	id<MTLBuffer> old = mBuffer;
	size_t oldsize = buffersize;
	Replace([fb->device newBufferWithLength:newsize options:MTLResourceStorageModeShared]);
	if (old)
		memcpy(mBuffer.contents, old.contents, std::min(oldsize, newsize));
	buffersize = newsize;
	map = mBuffer.contents;
}

void MtlHardwareBuffer::Map()
{
	if (mBuffer)
		map = mBuffer.contents;
}

void MtlHardwareBuffer::Unmap()
{
}

void *MtlHardwareBuffer::Lock(unsigned int size)
{
	size = std::max(size, (unsigned int)16);
	// Lock rewrites the contents: use new memory if the GPU may still read the old.
	if (!mBuffer || mBuffer.length < size || fb->GpuBusy())
		Allocate(size, nullptr);
	map = mBuffer.contents;
	return map;
}

void MtlHardwareBuffer::Unlock()
{
}

void MtlHardwareVertexBuffer::SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs)
{
	VertexFormat = fb->GetBufferManager()->GetVertexFormat(numBindingPoints, numAttributes, stride, attrs);
}

void MtlHardwareDataBuffer::BindRange(FRenderState *state, size_t start, size_t length)
{
	static_cast<MtlRenderState*>(state)->Bind(bindingpoint, (uint32_t)start);
}

/////////////////////////////////////////////////////////////////////////////

MtlStreamBuffer::MtlStreamBuffer(MetalRenderDevice *fb, size_t structSize, size_t count)
{
	mBlockSize = static_cast<uint32_t>((structSize + screen->uniformblockalignment - 1) / screen->uniformblockalignment * screen->uniformblockalignment);
	UniformBuffer = (MtlHardwareDataBuffer*)fb->GetBufferManager()->CreateDataBuffer(-1, false, false);
	UniformBuffer->FrameRing = true;
	UniformBuffer->SetData(mBlockSize * count, nullptr, BufferUsageType::Persistent);
}

MtlStreamBuffer::~MtlStreamBuffer()
{
	delete UniformBuffer;
}

uint32_t MtlStreamBuffer::NextStreamDataBlock()
{
	mStreamDataOffset += mBlockSize;
	if (mStreamDataOffset + (size_t)mBlockSize >= UniformBuffer->Size())
	{
		mStreamDataOffset = 0;
		return 0xffffffff;
	}
	return mStreamDataOffset;
}

/////////////////////////////////////////////////////////////////////////////

MtlBufferManager::MtlBufferManager(MetalRenderDevice *fb) : fb(fb)
{
}

MtlBufferManager::~MtlBufferManager()
{
	Deinit();
}

void MtlBufferManager::Init()
{
	MatrixBuffer.reset(new MtlStreamBuffer(fb, sizeof(MatricesUBO), 50000));
	StreamBuffer.reset(new MtlStreamBuffer(fb, sizeof(StreamUBO), 300));

	// Metal has no triangle fans; they are drawn as indexed triangle lists.
	TArray<uint32_t> data;
	for (int i = 2; i < 1000; i++)
	{
		data.Push(0);
		data.Push(i - 1);
		data.Push(i);
	}
	FanToTrisIndexBuffer.reset(CreateIndexBuffer());
	FanToTrisIndexBuffer->SetData(sizeof(uint32_t) * data.Size(), data.Data(), BufferUsageType::Static);
}

void MtlBufferManager::Deinit()
{
	FanToTrisIndexBuffer.reset();
	MatrixBuffer.reset();
	StreamBuffer.reset();
	while (!Buffers.empty())
		RemoveBuffer(Buffers.back());
}

void MtlBufferManager::AddBuffer(MtlHardwareBuffer *buffer)
{
	buffer->it = Buffers.insert(Buffers.end(), buffer);
}

void MtlBufferManager::RemoveBuffer(MtlHardwareBuffer *buffer)
{
	buffer->Reset();
	buffer->fb = nullptr;
	Buffers.erase(buffer->it);

	for (MtlHardwareDataBuffer **knownbuf : { &ViewpointUBO, &LightBufferSSO, &LightNodes, &LightLines, &LightList, &BoneBufferSSO })
	{
		if (buffer == *knownbuf) *knownbuf = nullptr;
	}
}

void MtlBufferManager::SetFrameSlot(int slot)
{
	for (auto buffer : Buffers)
		buffer->SetFrameSlot(slot);
}

IVertexBuffer *MtlBufferManager::CreateVertexBuffer()
{
	return new MtlHardwareVertexBuffer(fb);
}

IIndexBuffer *MtlBufferManager::CreateIndexBuffer()
{
	return new MtlHardwareIndexBuffer(fb);
}

IDataBuffer *MtlBufferManager::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	auto buffer = new MtlHardwareDataBuffer(fb, bindingpoint);
	switch (bindingpoint)
	{
	case LIGHTBUF_BINDINGPOINT: LightBufferSSO = buffer; buffer->FrameRing = true; break;
	case VIEWPOINT_BINDINGPOINT: ViewpointUBO = buffer; buffer->FrameRing = true; break;
	case LIGHTNODES_BINDINGPOINT: LightNodes = buffer; break;
	case LIGHTLINES_BINDINGPOINT: LightLines = buffer; break;
	case LIGHTLIST_BINDINGPOINT: LightList = buffer; break;
	case BONEBUF_BINDINGPOINT: BoneBufferSSO = buffer; buffer->FrameRing = true; break;
	default: break;
	}
	return buffer;
}

int MtlBufferManager::GetVertexFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs)
{
	for (size_t i = 0; i < VertexFormats.size(); i++)
	{
		const auto &f = VertexFormats[i];
		if (f.Attrs.size() == (size_t)numAttributes && f.NumBindingPoints == numBindingPoints && f.Stride == stride)
		{
			bool matches = true;
			for (int j = 0; j < numAttributes; j++)
			{
				if (memcmp(&f.Attrs[j], &attrs[j], sizeof(FVertexBufferAttribute)) != 0)
				{
					matches = false;
					break;
				}
			}
			if (matches)
				return (int)i;
		}
	}

	MtlVertexFormat fmt;
	fmt.NumBindingPoints = numBindingPoints;
	fmt.Stride = stride;
	fmt.UseVertexData = 0;
	for (int j = 0; j < numAttributes; j++)
	{
		if (attrs[j].location == VATTR_COLOR)
			fmt.UseVertexData |= 1;
		else if (attrs[j].location == VATTR_NORMAL)
			fmt.UseVertexData |= 2;
		fmt.Attrs.push_back(attrs[j]);
	}
	VertexFormats.push_back(fmt);
	return (int)VertexFormats.size() - 1;
}
