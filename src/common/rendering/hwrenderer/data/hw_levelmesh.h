#pragma once

#include "tarray.h"
#include "vectors.h"
#include <atomic>
#include <stdint.h>

namespace hwrenderer
{

class LevelMesh
{
public:
	LevelMesh() : Generation(NextGeneration()) { }
	virtual ~LevelMesh() = default;

	TArray<FVector3> MeshVertices;
	TArray<int> MeshUVIndex;
	TArray<uint32_t> MeshElements;
	TArray<int> MeshSurfaces;

	// The triangles of MeshElements that block ray traced shadows. Sky and
	// see-through surfaces only take part in the baked lighting.
	TArray<uint32_t> ShadowElements;

	// Unique for every mesh ever built. Backends cache acceleration structures on
	// it rather than on the mesh address, which the allocator may reuse.
	const uint64_t Generation;

private:
	static uint64_t NextGeneration()
	{
		static std::atomic<uint64_t> counter{ 0 };
		return ++counter;
	}
};

} // namespace
