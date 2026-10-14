#include "watch_bridge_internal.h"
#include "v_video.h"
#include "i_video.h"
#include "v_2ddrawer.h"
#include "v_draw.h"
#include "textures.h"
#include "gametexture.h"
#include "bitmap.h"
#include "c_cvars.h"
#include "printf.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>

IVideo *Video;
EXTERN_CVAR(Int, vid_rendermode)
EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Int, vid_scalemode)
EXTERN_CVAR(Float, vid_scalefactor)

CVARD(Bool, watch_post, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "watch: bloom, filmic contrast and vignette on the final image")
CVARD(Float, watch_renderscale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "watch: render width relative to the native screen width (0.5-1, applied at startup)")

namespace
{
using Vertex = F2DDrawer::TwoDVertex;
struct Picture
{
    int width, height;
    std::vector<PalEntry> pixels;
};

float Edge(const Vertex &a, const Vertex &b, float x, float y)
{
    return (x-a.x)*(b.y-a.y) - (y-a.y)*(b.x-a.x);
}
int Limit(float value) { return std::clamp(int(value), 0, 255); }
float BlendFactor(int mode, float alpha, float source, float destination)
{
    switch (mode)
    {
        case STYLEALPHA_Zero: return 0;
        case STYLEALPHA_One: return 1;
        case STYLEALPHA_Src: return alpha;
        case STYLEALPHA_InvSrc: return 1-alpha;
        case STYLEALPHA_SrcCol: return source/255;
        case STYLEALPHA_InvSrcCol: return 1-source/255;
        case STYLEALPHA_DstCol: return destination/255;
        case STYLEALPHA_InvDstCol: return 1-destination/255;
        case STYLEALPHA_Dst: return 1;
        case STYLEALPHA_InvDst: return 0;
    }
    return alpha;
}

// The render width follows the watch's native pixel width so the image is shown
// 1:1 instead of being stretched up from 320x240. The view stays 4:3.
void RenderSize(int &width, int &height)
{
    int screenWidth, screenHeight;
    WatchScreenSize(screenWidth, screenHeight);
    if (screenWidth <= 0) screenWidth = 320;
    float scale = std::clamp(float(watch_renderscale), 0.5f, 1.f);
    width = std::clamp(int(screenWidth * scale) & ~3, 320, 480);
    height = (width * 3 / 4) & ~1;
}

// Post-processing on the CPU, sized for a watch: a quarter-resolution bloom,
// a filmic contrast curve with a little extra saturation, and a vignette.
// About 20 operations per pixel.
class WatchPost
{
    int width = 0, height = 0;
    int bloomWidth = 0, bloomHeight = 0;
    std::vector<float> bloom, scratch;
    std::vector<uint16_t> vignette;
    uint8_t curve[256];
    std::vector<PalEntry> output;

    void Resize(int w, int h)
    {
        width = w; height = h;
        bloomWidth = (w + 3) / 4; bloomHeight = (h + 3) / 4;
        bloom.assign(size_t(bloomWidth) * bloomHeight * 3, 0.f);
        scratch.assign(bloom.size(), 0.f);
        output.resize(size_t(w) * h);
        vignette.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        {
            float dx = (x + .5f) / w - .5f, dy = ((y + .5f) / h - .5f) * .75f;
            float d = std::sqrt(dx * dx + dy * dy) * 1.6f;
            float v = 1.f - .28f * std::clamp((d - .35f) / .55f, 0.f, 1.f) * std::clamp((d - .35f) / .55f, 0.f, 1.f);
            vignette[size_t(y) * w + x] = uint16_t(v * 256);
        }
        for (int i = 0; i < 256; ++i)
        {
            float c = i / 255.f;
            float s = c * c * (3.f - 2.f * c);
            curve[i] = uint8_t(std::clamp(c + (s - c) * .4f, 0.f, 1.f) * 255.f + .5f);
        }
    }

    void Blur(std::vector<float> &src, std::vector<float> &dst, int dx, int dy)
    {
        for (int y = 0; y < bloomHeight; ++y) for (int x = 0; x < bloomWidth; ++x)
        {
            int x0 = std::max(x - dx, 0), x1 = std::min(x + dx, bloomWidth - 1);
            int y0 = std::max(y - dy, 0), y1 = std::min(y + dy, bloomHeight - 1);
            for (int c = 0; c < 3; ++c)
                dst[(size_t(y) * bloomWidth + x) * 3 + c] = .25f * src[(size_t(y0) * bloomWidth + x0) * 3 + c]
                    + .5f * src[(size_t(y) * bloomWidth + x) * 3 + c] + .25f * src[(size_t(y1) * bloomWidth + x1) * 3 + c];
        }
    }

public:
    const uint8_t *Process(const PalEntry *src, int w, int h, int pitch)
    {
        if (w != width || h != height) Resize(w, h);

        // Bright pass at quarter resolution.
        for (int by = 0; by < bloomHeight; ++by) for (int bx = 0; bx < bloomWidth; ++bx)
        {
            float sum[3] = {0, 0, 0};
            int n = 0;
            for (int y = by * 4; y < std::min(by * 4 + 4, h); ++y) for (int x = bx * 4; x < std::min(bx * 4 + 4, w); ++x)
            {
                PalEntry p = src[size_t(y) * pitch + x];
                sum[0] += p.r; sum[1] += p.g; sum[2] += p.b; ++n;
            }
            float r = sum[0] / (n * 255.f), g = sum[1] / (n * 255.f), b = sum[2] / (n * 255.f);
            float peak = std::max({r, g, b});
            float k = std::clamp((peak - .62f) / .38f, 0.f, 1.f);
            k *= k;
            float *out = &bloom[(size_t(by) * bloomWidth + bx) * 3];
            out[0] = r * k; out[1] = g * k; out[2] = b * k;
        }
        for (int pass = 1; pass <= 2; ++pass)
        {
            Blur(bloom, scratch, pass, 0);
            Blur(scratch, bloom, 0, pass);
        }

        for (int y = 0; y < h; ++y)
        {
            float fy = std::clamp((y + .5f) / 4.f - .5f, 0.f, float(bloomHeight - 1));
            int y0 = int(fy), y1 = std::min(y0 + 1, bloomHeight - 1);
            float ty = fy - y0;
            for (int x = 0; x < w; ++x)
            {
                float fx = std::clamp((x + .5f) / 4.f - .5f, 0.f, float(bloomWidth - 1));
                int x0 = int(fx), x1 = std::min(x0 + 1, bloomWidth - 1);
                float tx = fx - x0;
                const float *b00 = &bloom[(size_t(y0) * bloomWidth + x0) * 3], *b10 = &bloom[(size_t(y0) * bloomWidth + x1) * 3];
                const float *b01 = &bloom[(size_t(y1) * bloomWidth + x0) * 3], *b11 = &bloom[(size_t(y1) * bloomWidth + x1) * 3];
                PalEntry p = src[size_t(y) * pitch + x];
                float c[3] = {float(p.r), float(p.g), float(p.b)};
                float luma = c[0] * .2126f + c[1] * .7152f + c[2] * .0722f;
                int v = vignette[size_t(y) * w + x];
                int result[3];
                for (int i = 0; i < 3; ++i)
                {
                    float glow = (b00[i] * (1 - tx) + b10[i] * tx) * (1 - ty) + (b01[i] * (1 - tx) + b11[i] * tx) * ty;
                    float value = luma + (c[i] - luma) * 1.12f + glow * 150.f;
                    int t = curve[std::clamp(int(value), 0, 255)];
                    result[i] = (t * v) >> 8;
                }
                output[size_t(y) * w + x] = PalEntry(255, result[0], result[1], result[2]);
            }
        }
        return reinterpret_cast<const uint8_t *>(output.data());
    }
};

class WatchFrameBuffer final : public DFrameBuffer
{
    DCanvas canvas;
    WatchPost post;
    std::unordered_map<FTexture *, Picture> pictures;
    std::chrono::steady_clock::time_point nextFrame;

    Picture *GetPicture(FGameTexture *game)
    {
        if (!game || !game->GetTexture()) return nullptr;
        auto texture = game->GetTexture();
        auto found = pictures.find(texture);
        if (found != pictures.end()) return &found->second;
        FBitmap image = texture->GetBgraBitmap(nullptr);
        Picture pic;
        pic.width = image.GetWidth(); pic.height = image.GetHeight();
        if (pic.width < 1 || pic.height < 1 || !image.GetPixels()) return nullptr;
        pic.pixels.resize(size_t(pic.width) * pic.height);
        for (int y=0; y<pic.height; ++y)
            memcpy(pic.pixels.data() + size_t(y)*pic.width, image.GetPixels() + size_t(y)*image.GetPitch(), size_t(pic.width)*4);
        return &pictures.emplace(texture, std::move(pic)).first->second;
    }

    void Pixel(int x, int y, PalEntry color, const F2DDrawer::RenderCommand &cmd)
    {
        if (x<0 || y<0 || x>=GetWidth() || y>=GetHeight()) return;
        auto &dst = reinterpret_cast<PalEntry *>(canvas.GetPixels())[y*canvas.GetPitch()+x];
        float alpha = color.a / 255.f;
        if (alpha == 0) return;
        float source[3] = {float(color.r), float(color.g), float(color.b)};
        float dest[3] = {float(dst.r), float(dst.g), float(dst.b)};
        int result[3];
        for (int c=0; c<3; ++c)
        {
            float a = source[c] * BlendFactor(cmd.mRenderStyle.SrcAlpha, alpha, source[c], dest[c]);
            float b = dest[c] * BlendFactor(cmd.mRenderStyle.DestAlpha, alpha, source[c], dest[c]);
            result[c] = Limit(cmd.mRenderStyle.BlendOp == STYLEOP_Sub ? a-b :
                cmd.mRenderStyle.BlendOp == STYLEOP_RevSub ? b-a : a+b);
        }
        dst = PalEntry(255, result[0], result[1], result[2]);
    }

    void Triangle(const Vertex &a, const Vertex &b, const Vertex &c, const F2DDrawer::RenderCommand &cmd, Picture *pic)
    {
        float area = Edge(a,b,c.x,c.y);
        if (std::abs(area) < .00001f) return;
        int x0 = std::max(0, int(std::floor(std::min({a.x,b.x,c.x}))));
        int y0 = std::max(0, int(std::floor(std::min({a.y,b.y,c.y}))));
        int x1 = std::min(GetWidth(), int(std::ceil(std::max({a.x,b.x,c.x}))));
        int y1 = std::min(GetHeight(), int(std::ceil(std::max({a.y,b.y,c.y}))));
        if (cmd.mFlags & F2DDrawer::DTF_Scissor)
        {
            x0=std::max(x0,cmd.mScissor[0]); y0=std::max(y0,cmd.mScissor[1]);
            x1=std::min(x1,cmd.mScissor[2]); y1=std::min(y1,cmd.mScissor[3]);
        }
        for (int y=y0; y<y1; ++y) for (int x=x0; x<x1; ++x)
        {
            float wa=Edge(b,c,x+.5f,y+.5f)/area;
            float wb=Edge(c,a,x+.5f,y+.5f)/area;
            float wc=1-wa-wb;
            if (wa<0 || wb<0 || wc<0) continue;
            PalEntry texel(255,255,255,255);
            if (pic)
            {
                float u=a.u*wa+b.u*wb+c.u*wc, v=a.v*wa+b.v*wb+c.v*wc;
                if (cmd.mFlags & F2DDrawer::DTF_Wrap) { u-=std::floor(u); v-=std::floor(v); }
                int tx=std::clamp(int(u*pic->width),0,pic->width-1);
                int ty=std::clamp(int(v*pic->height),0,pic->height-1);
                texel=pic->pixels[size_t(ty)*pic->width+tx];
            }
            if (cmd.mDrawMode == TM_OPAQUE) texel.a=255;
            if (cmd.mDrawMode == TM_STENCIL || (cmd.mRenderStyle.Flags & STYLEF_ColorIsFixed))
                texel.r=texel.g=texel.b=255;
            PalEntry color;
            color.a=Limit(texel.a*(a.color0.a*wa+b.color0.a*wb+c.color0.a*wc)/255);
            color.r=Limit(texel.r*(a.color0.r*wa+b.color0.r*wb+c.color0.r*wc)/255);
            color.g=Limit(texel.g*(a.color0.g*wa+b.color0.g*wb+c.color0.g*wc)/255);
            color.b=Limit(texel.b*(a.color0.b*wa+b.color0.b*wb+c.color0.b*wc)/255);
            float overlay=cmd.mColor1.a/255.f;
            color.r=Limit(color.r*(1-overlay)+cmd.mColor1.r*overlay);
            color.g=Limit(color.g*(1-overlay)+cmd.mColor1.g*overlay);
            color.b=Limit(color.b*(1-overlay)+cmd.mColor1.b*overlay);
            Pixel(x,y,color,cmd);
        }
    }
public:
    WatchFrameBuffer(int width, int height) : DFrameBuffer(width, height), canvas(width, height, true) { vendorstring="watchOS software"; }
    void InitializeState() override { SetViewportRects(nullptr); }
    bool IsPoly() override { return true; }
    bool IsFullscreen() override { return true; }
    DCanvas *GetCanvas() override { return &canvas; }
    int GetClientWidth() override { return GetWidth(); }
    int GetClientHeight() override { return GetHeight(); }
    const char *DeviceName() const override { return "watchOS software framebuffer"; }
    void Draw2D() override
    {
        for (auto &cmd : twod->mData)
        {
            if (cmd.isSpecial != SpecialDrawCommand::NotSpecial) continue;
            Picture *pic=GetPicture(cmd.mTexture);
            if (cmd.mType==F2DDrawer::DrawTypeTriangles)
            {
                for (int i=0; i+2<cmd.mIndexCount; i+=3)
                {
                    const Vertex &a=twod->mVertices[twod->mIndices[cmd.mIndexIndex+i]];
                    const Vertex &b=twod->mVertices[twod->mIndices[cmd.mIndexIndex+i+1]];
                    const Vertex &c=twod->mVertices[twod->mIndices[cmd.mIndexIndex+i+2]];
                    Triangle(a,b,c,cmd,pic);
                }
            }
            else if (cmd.mType==F2DDrawer::DrawTypeLines)
            {
                for (int i=0; i+1<cmd.mVertCount; i+=2)
                {
                    const Vertex &a=twod->mVertices[cmd.mVertIndex+i], &b=twod->mVertices[cmd.mVertIndex+i+1];
                    int steps=std::max(1,int(std::max(std::abs(b.x-a.x),std::abs(b.y-a.y))));
                    for (int j=0; j<=steps; ++j)
                        Pixel(int(a.x+(b.x-a.x)*j/steps),int(a.y+(b.y-a.y)*j/steps),a.color0,cmd);
                }
            }
            else if (cmd.mType==F2DDrawer::DrawTypePoints)
                for (int i=0; i<cmd.mVertCount; ++i)
                {
                    const Vertex &a=twod->mVertices[cmd.mVertIndex+i];
                    Pixel(int(a.x),int(a.y),a.color0,cmd);
                }
        }
    }
    void Update() override
    {
        // Startup screens and HUDs submit 2D commands outside RenderView. Consume
        // them while their textures are alive, then release the command list.
        Draw2D();
        twod->Clear();
        if (watch_post)
            WatchPresent(post.Process(reinterpret_cast<const PalEntry *>(canvas.GetPixels()), GetWidth(), GetHeight(), canvas.GetPitch()), GetWidth(), GetHeight(), GetWidth());
        else
            WatchPresent(canvas.GetPixels(),GetWidth(),GetHeight(),canvas.GetPitch());
        // Sleeping through the whole frame budget avoids desktop-style spinning
        // near the deadline and permits rendering below the 35 Hz game tick rate.
        auto now=std::chrono::steady_clock::now();
        auto interval=std::chrono::nanoseconds(1000000000 / WatchFrameLimit());
        if (nextFrame<now-interval || nextFrame>now+interval*2) nextFrame=now;
        nextFrame+=interval;
        std::this_thread::sleep_until(nextFrame);
        DFrameBuffer::Update();
    }
    TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &type, float &gamma) override
    {
        pitch=GetWidth()*3; type=SS_RGB; gamma=1;
        TArray<uint8_t> output;
        output.Resize(pitch*GetHeight());
        auto pixels=reinterpret_cast<PalEntry *>(canvas.GetPixels());
        for (int y=0; y<GetHeight(); ++y) for (int x=0; x<GetWidth(); ++x)
        {
            auto p=pixels[y*canvas.GetPitch()+x];
            int i=y*pitch+x*3;
            output[i]=p.r; output[i+1]=p.g; output[i+2]=p.b;
        }
        return output;
    }
};
class WatchVideo final : public IVideo
{
public:
    DFrameBuffer *CreateFrameBuffer() override
    {
        int width, height;
        RenderSize(width, height);
        Printf("watchOS: %dx%d true-color software renderer\n", width, height);
        return new WatchFrameBuffer(width, height);
    }
};
}

void I_InitGraphics()
{
    vid_rendermode=1;
    vid_maxfps=35;
    vid_scalemode=0;
    vid_scalefactor=1;
    Video=new WatchVideo;
}
void I_ShutdownGraphics() { delete screen; screen=nullptr; delete Video; Video=nullptr; }
