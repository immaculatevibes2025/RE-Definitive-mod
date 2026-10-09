// MarniDX.cpp - D3D11 backend implementation for the Marni System shim.
//
// This file is the ONLY translation unit in the project that owns D3D11,
// DXGI, and the HLSL/D3DCompiler pipeline. Everything exposed to the game
// layer goes through the MarniDX class declared in MarniDX.h, using opaque
// MarniHandle texture handles. No <d3d11.h>-derived type is visible outside
// this file (verified: none of the game-layer headers include d3d11.h).
//
// The body is a faithful re-home of the pipeline that used to live mixed into
// MarniSystem.cpp / MarniBits.cpp / CharacterSelectionScreen.cpp, with the
// raw COM pointers moved into a single private Impl. Behaviour is unchanged
// from the previous working build — just re-encapsulated.
#include "MarniDX.h"

#include <new>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "../game/Types.h"
// SMAA (Jorge Jimenez et al., MIT license - see smaa/LICENSE.txt): the shader
// source and its two precomputed lookup textures.
#include "smaa/SmaaHlsl.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

// config.ini [Display] VSync (Globals.cpp). Declared locally rather than
// pulling all of Globals.h into the Marni layer.
extern BOOL g_bVSync;
// CrashLog.cpp: one "[step]" line in crash.log next to the exe.
extern void crashlog_mark(const char* step);
static int s_postTrace = 0;     // log the first few post-pass frames after a rebuild

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

// Forward-declare the game-layer structs we only pass by opaque pointer.
struct DisplayModeInfo;
struct D3DRendererInfo;

// ============================================================================
// Shader sources (compiled at runtime via D3DCompile). Identical to the
// previous build's embedded sources so behaviour is byte-for-byte the same.
// ============================================================================
static const char* g_QuadVS_Source = R"(
cbuffer SpriteCB : register(b0) { row_major float4x4 g_MVP; };
struct VS_INPUT  { float2 pos:POSITION; float2 tex:TEXCOORD0; float4 col:COLOR0; };
struct VS_OUTPUT { float4 pos:SV_Position; float2 tex:TEXCOORD0; float4 col:COLOR0; };
VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT o;
    o.pos = mul(float4(input.pos.x, input.pos.y, 0.0f, 1.0f), g_MVP);
    o.tex = input.tex;
    o.col = input.col;
    return o;
}
)";

// 3D model VS: same screen-space ortho for x/y, but the vertex carries a
// pre-normalised [0,1] depth that goes straight into NDC z so the depth buffer
// can resolve the model. The original leaned on a real Z-buffer for this
// ("MarniSystem Direct3D::MD3DCreateZBuffer"); a per-triangle painter sort
// cannot, and made faces pop in and out as a model turned.
//
// pos.w carries the vertex's VIEW-SPACE Z. The caller already did the divide
// when it projected to screen space, so emitting SV_Position with w = 1 made
// the rasteriser interpolate UV and colour LINEARLY IN SCREEN SPACE - affine
// mapping, i.e. the PS1 texture swim. Pre-multiplying x/y/z by w and handing
// the rasteriser that w restores the same screen position after the perspective
// divide while making every interpolator perspective-correct. It shows up
// worst on big polygons close to the camera at an oblique angle - the door
// panel in the room-transition animation.
static const char* g_Model3DVS_Source = R"(
cbuffer SpriteCB : register(b0) { row_major float4x4 g_MVP; };
struct VS_INPUT  { float4 pos:POSITION; float2 tex:TEXCOORD0; float4 col:COLOR0; };
struct VS_OUTPUT { float4 pos:SV_Position; float2 tex:TEXCOORD0; float4 col:COLOR0; };
VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT o;
    float4 p = mul(float4(input.pos.x, input.pos.y, 0.0f, 1.0f), g_MVP);
    float w = max(input.pos.w, 1e-4f);
    o.pos = float4(p.x * w, p.y * w, input.pos.z * w, w);
    o.tex = input.tex;
    o.col = input.col;
    return o;
}
)";

static const char* g_QuadPS_Source = R"(
Texture2D    g_Texture : register(t0);
SamplerState g_Sampler  : register(s0);
struct PS_INPUT { float4 pos:SV_Position; float2 tex:TEXCOORD0; float4 col:COLOR0; };
float4 main(PS_INPUT i) : SV_Target { return g_Texture.Sample(g_Sampler, i.tex) * i.col; }
)";

// ============================================================================
// Port-added post-processing (Video Options: ANTI-ALIASING / CRT SHADER).
// With either on, the game draws into an offscreen scene target and Present()
// puts it on the back buffer: SMAA's three passes when anti-aliasing is on,
// then (with the CRT shader, or without SMAA) one full-screen pass - a
// straight copy, or the CRT shader below.
// ============================================================================
static const char* g_PostVS_Source = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

static const char* g_CopyPS_Source = R"(
Texture2D g_Scene : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    return float4(g_Scene.Load(int3(int2(pos.xy), 0)).rgb, 1.0);
}
)";

// CRT shader in the style of CRT-Royale (TroggleMonkey, libretro). A
// single-pass re-implementation of its main ideas, not the multi-pass
// original: linear-light gaussian scanline beams whose width grows with
// brightness, a horizontal beam blur, an aperture-grille phosphor mask at
// the output resolution with brightness compensation, halation, gentle
// curvature with rounded corners, and a vignette. The scanlines follow the
// game's own 240 lines inside the content rectangle.
static const char* g_CrtPS_Source = R"(
Texture2D    g_Scene   : register(t0);
SamplerState g_Linear  : register(s0);
cbuffer CrtCB : register(b0)
{
    float4 g_Rect;     // content rectangle in back-buffer pixels: x, y, w, h
    float4 g_Size;     // back buffer w, h, 1/w, 1/h
    float4 g_Src;      // source lines: 320, 240 (fewer when 16:9 crops), 0, 0
};

#define SRC (g_Src.xy)
static const float  GAMMA_IN     = 2.4;
static const float  GAMMA_OUT    = 2.2;
static const float  SIGMA_MIN    = 0.20;   // beam half-width at black (in lines)
static const float  SIGMA_MAX    = 0.42;   // beam half-width at full white
static const float  MASK_DIM     = 0.40;   // aperture grille: off-phosphor level
static const float  MASK_BOOST   = 1.55;   // brightness given back after the mask
static const float  HALATION     = 0.08;
static const float2 CURVATURE    = float2(0.030, 0.042);
static const float  CORNER       = 0.012;

float3 ToLinear(float3 c) { return pow(max(c, 0.0), GAMMA_IN); }

float2 Curve(float2 uv)
{
    float2 c = uv * 2.0 - 1.0;
    c *= 1.0 + c.yx * c.yx * CURVATURE;
    return c * 0.5 + 0.5;
}

// One source line, sampled at x with a 3-tap horizontal beam blur.
float3 Line(float row, float x)
{
    row = clamp(row, 0.0, SRC.y - 1.0);
    float  v  = (row + 0.5) / SRC.y;
    float  dx = 0.55 / SRC.x;
    float2 a  = (g_Rect.xy + float2(x - dx, v) * g_Rect.zw) * g_Size.zw;
    float2 b  = (g_Rect.xy + float2(x,      v) * g_Rect.zw) * g_Size.zw;
    float2 c  = (g_Rect.xy + float2(x + dx, v) * g_Rect.zw) * g_Size.zw;
    return ToLinear(g_Scene.SampleLevel(g_Linear, a, 0).rgb) * 0.25 +
           ToLinear(g_Scene.SampleLevel(g_Linear, b, 0).rgb) * 0.50 +
           ToLinear(g_Scene.SampleLevel(g_Linear, c, 0).rgb) * 0.25;
}

// Gaussian beam profile, per channel, wider for brighter colour.
float3 Beam(float dist, float3 col)
{
    float3 sigma = lerp(SIGMA_MIN, SIGMA_MAX, sqrt(saturate(col)));
    float3 d = dist / sigma;
    return exp(-0.5 * d * d) * (0.40 / sigma);
}

float4 main(float4 pos : SV_Position) : SV_Target
{
    float2 uv = (pos.xy - g_Rect.xy) / g_Rect.zw;
    if (any(uv < 0.0) || any(uv > 1.0)) return float4(0, 0, 0, 1);

    uv = Curve(uv);
    float2 edge = min(uv, 1.0 - uv);
    if (any(edge < 0.0)) return float4(0, 0, 0, 1);
    float corner = saturate(min(edge.x, edge.y) / CORNER);

    float y   = uv.y * SRC.y - 0.5;
    float row = floor(y);
    float f   = y - row;

    float3 c0 = Line(row,       uv.x);
    float3 c1 = Line(row + 1.0, uv.x);
    float3 col = c0 * Beam(f, c0) + c1 * Beam(1.0 - f, c1);

    // Halation: light spreading in the glass, from a wide cross of taps.
    float3 glow = 0.0;
    [unroll] for (int i = -2; i <= 2; i++) {
        glow += Line(row + i * 2.0, uv.x);
        glow += Line(row, uv.x + i * 3.0 / SRC.x);
    }
    col = lerp(col, glow / 10.0, HALATION);

    // Aperture grille: R, G, B stripes one output pixel wide.
    int    m    = (int)pos.x % 3;
    float3 mask = MASK_DIM;
    if (m == 0) mask.r = 1.0; else if (m == 1) mask.g = 1.0; else mask.b = 1.0;
    col *= mask * MASK_BOOST;

    // Vignette and the rounded corners.
    float2 vc = uv * (1.0 - uv.yx);
    col *= pow(saturate(vc.x * vc.y * 15.0), 0.18) * corner;

    return float4(pow(saturate(col), 1.0 / GAMMA_OUT), 1.0);
}
)";

// SMAA 1x around SMAA.hlsl (smaa/SmaaHlsl.h): CompilePostShaders builds the
// source as prefix + SMAA.hlsl + suffix and compiles one entry point per pass.
static const char* g_SmaaPrefix_Source = R"SMAA(
// SMAA 1x (High preset) for the post pass. SMAA.hlsl is compiled with
// SMAA_CUSTOM_SL so its samplers sit at fixed registers (s0 linear, s1 point)
// instead of the effect-framework sampler blocks of SMAA_HLSL_4.
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
cbuffer SmaaCB : register(b0) { float4 g_SmaaMetrics; };
#define SMAA_RT_METRICS g_SmaaMetrics
SamplerState SmaaLinear : register(s0);
SamplerState SmaaPoint  : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(SmaaLinear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(SmaaPoint, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(SmaaLinear, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(SmaaLinear, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(SmaaPoint, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(SmaaLinear, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)SMAA";

static const char* g_SmaaSuffix_Source = R"SMAA(
// Full-screen triangle; uv (0,0) is the top-left of the scene.
void SmaaTri(uint id, out float4 pos, out float2 uv)
{
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Pass 1: luma edges of the scene (t0) into an RG8 target.
Texture2D g_SmaaColor  : register(t0);
void SmaaEdgeVS(uint id : SV_VertexID, out float4 pos : SV_Position,
                out float2 uv : TEXCOORD0, out float4 off[3] : TEXCOORD1)
{
    SmaaTri(id, pos, uv);
    SMAAEdgeDetectionVS(uv, off);
}
float4 SmaaEdgePS(float4 pos : SV_Position, float2 uv : TEXCOORD0,
                  float4 off[3] : TEXCOORD1) : SV_Target
{
    return float4(SMAALumaEdgeDetectionPS(uv, off, g_SmaaColor), 0.0, 0.0);
}

// Pass 2: blending weights from the edges (t1) and the lookup textures.
Texture2D g_SmaaEdges  : register(t1);
Texture2D g_SmaaArea   : register(t2);
Texture2D g_SmaaSearch : register(t3);
void SmaaWeightVS(uint id : SV_VertexID, out float4 pos : SV_Position,
                  out float2 uv : TEXCOORD0, out float2 pix : TEXCOORD1,
                  out float4 off[3] : TEXCOORD2)
{
    SmaaTri(id, pos, uv);
    SMAABlendingWeightCalculationVS(uv, pix, off);
}
float4 SmaaWeightPS(float4 pos : SV_Position, float2 uv : TEXCOORD0,
                    float2 pix : TEXCOORD1, float4 off[3] : TEXCOORD2) : SV_Target
{
    return SMAABlendingWeightCalculationPS(uv, pix, off, g_SmaaEdges,
                                           g_SmaaArea, g_SmaaSearch, 0.0);
}

// Pass 3: blend the scene (t0) with its neighbours by the weights (t4).
Texture2D g_SmaaBlend  : register(t4);
void SmaaBlendVS(uint id : SV_VertexID, out float4 pos : SV_Position,
                 out float2 uv : TEXCOORD0, out float4 off : TEXCOORD1)
{
    SmaaTri(id, pos, uv);
    SMAANeighborhoodBlendingVS(uv, off);
}
float4 SmaaBlendPS(float4 pos : SV_Position, float2 uv : TEXCOORD0,
                   float4 off : TEXCOORD1) : SV_Target
{
    return float4(SMAANeighborhoodBlendingPS(uv, off, g_SmaaColor, g_SmaaBlend).rgb, 1.0);
}
)SMAA";

struct SmaaConstantBuffer {
    float metrics[4];   // 1/width, 1/height, width, height
};

struct CrtConstantBuffer {
    float rect[4];
    float size[4];
    float src[4];      // mod: source resolution the scanlines follow
};

// ============================================================================
// Internal types (file-local, never exported)
// ============================================================================
struct QuadVertex {
    float x, y;         // screen position
    float u, v;         // texture coords
    float r, g, b, a;   // tint color 0..1
};

// Depth-buffered variant used by the TMD model path.
struct Model3DVertex {
    float x, y, z;      // screen position + normalised [0,1] depth
    float w;            // view-space Z; the VS uses it to restore perspective-
                        // correct interpolation of u/v and the vertex colour
    float u, v;
    float r, g, b, a;
};

struct SpriteConstantBuffer {
    float mvp[4][4];
};

// Handle-table slot: maps a public MarniHandle (1-based index) to its D3D11
// objects. Slot 0 is reserved for MARNI_NULL_HANDLE.
struct TexSlot {
    ID3D11Texture2D*        tex;
    ID3D11ShaderResourceView* srv;
    int                     width;
    int                     height;
};

#define MARNI_MAX_TEXTURES 2048

// ============================================================================
// MarniDX::Impl - all D3D11 ownership lives here.
// ============================================================================
struct MarniDX::Impl {
    // core device
    ID3D11Device*           device    = nullptr;
    ID3D11DeviceContext*    context   = nullptr;
    IDXGISwapChain*          swapChain = nullptr;
    HWND                     hWnd      = nullptr;

    // render target
    ID3D11RenderTargetView*  rtv       = nullptr;
    ID3D11Texture2D*         depthStencil    = nullptr;
    ID3D11DepthStencilView*   depthStencilView = nullptr;

    // Port-added: the target the game is drawing into this frame - the back
    // buffer above, or the offscreen scene below. Non-owning aliases.
    ID3D11RenderTargetView*  curRtv        = nullptr;
    ID3D11DepthStencilView*  curDsv        = nullptr;

    // Port-added offscreen scene (SMAA and/or CRT shader). Back-buffer sized.
    BOOL                     smaa          = FALSE; // requested
    BOOL                     smaaActive    = FALSE; // the SMAA targets exist
    BOOL                     crtShader     = FALSE;
    ID3D11Texture2D*         sceneTex      = nullptr;
    ID3D11RenderTargetView*  sceneRtv      = nullptr;
    ID3D11ShaderResourceView* sceneSrv     = nullptr;
    ID3D11Texture2D*         sceneDepth    = nullptr;
    ID3D11DepthStencilView*  sceneDsv      = nullptr;
    ID3D11VertexShader*      postVS        = nullptr;
    ID3D11PixelShader*       copyPS        = nullptr;
    ID3D11PixelShader*       crtPS         = nullptr;
    ID3D11Buffer*            crtCB         = nullptr;
    ID3D11RasterizerState*   rasterNoScissor = nullptr;

    // SMAA: per-scene targets (edges RG8, blend weights RGBA8, and the
    // anti-aliased frame when the CRT shader still has to run on it), the
    // device-lifetime lookup textures, and the three passes' shaders.
    ID3D11Texture2D*         smaaEdgesTex  = nullptr;
    ID3D11RenderTargetView*  smaaEdgesRtv  = nullptr;
    ID3D11ShaderResourceView* smaaEdgesSrv = nullptr;
    ID3D11Texture2D*         smaaBlendTex  = nullptr;
    ID3D11RenderTargetView*  smaaBlendRtv  = nullptr;
    ID3D11ShaderResourceView* smaaBlendSrv = nullptr;
    ID3D11Texture2D*         smaaOutTex    = nullptr;   // only with the CRT shader
    ID3D11RenderTargetView*  smaaOutRtv    = nullptr;
    ID3D11ShaderResourceView* smaaOutSrv   = nullptr;
    ID3D11Texture2D*         smaaAreaTex   = nullptr;
    ID3D11ShaderResourceView* smaaAreaSrv  = nullptr;
    ID3D11Texture2D*         smaaSearchTex = nullptr;
    ID3D11ShaderResourceView* smaaSearchSrv = nullptr;
    ID3D11VertexShader*      smaaEdgeVS    = nullptr;
    ID3D11PixelShader*       smaaEdgePS    = nullptr;
    ID3D11VertexShader*      smaaWeightVS  = nullptr;
    ID3D11PixelShader*       smaaWeightPS  = nullptr;
    ID3D11VertexShader*      smaaBlendVS   = nullptr;
    ID3D11PixelShader*       smaaBlendPS   = nullptr;
    ID3D11Buffer*            smaaCB        = nullptr;

    BOOL UsesScene() const { return smaa || crtShader; }
    BOOL SmaaReady() const {
        return smaaEdgeVS && smaaEdgePS && smaaWeightVS && smaaWeightPS &&
               smaaBlendVS && smaaBlendPS && smaaCB && smaaAreaSrv && smaaSearchSrv;
    }

    // Set by SetSmaa / SetCrtShader; the scene is rebuilt at the end of the
    // next Present(). See SetSmaa for why it is never rebuilt on the spot.
    BOOL                     sceneDirty    = FALSE;

    // pipeline state
    ID3D11RasterizerState*   rasterScissor = nullptr;
    ID3D11BlendState*        blendAlpha    = nullptr;
    ID3D11BlendState*        blendAdd      = nullptr;
    ID3D11BlendState*        blendDisabled = nullptr;
    ID3D11SamplerState*      sampLinear    = nullptr;
    ID3D11SamplerState*      sampPoint     = nullptr;
    ID3D11DepthStencilState* depthDisabled = nullptr;
    ID3D11DepthStencilState* depthEnabled  = nullptr;
    // Test-only depth (write off) for the translucent ground-shadow / blood
    // pool quads: the original rendered them as world-space viewport quads
    // against the same Z-buffer as the TMD objects, so a character in front
    // clipped them no matter where the ordering table put the primitive.
    ID3D11DepthStencilState* depthTestNoWrite = nullptr;

    // shaders / buffers
    ID3D11VertexShader*      quadVS        = nullptr;
    ID3D11PixelShader*       quadPS        = nullptr;
    ID3D11InputLayout*       quadLayout    = nullptr;
    ID3D11Buffer*            quadVB        = nullptr;
    ID3D11Buffer*            spriteCB      = nullptr;

    // depth-buffered 3D model pipeline (shares quadPS and spriteCB)
    ID3D11VertexShader*      model3DVS     = nullptr;
    ID3D11InputLayout*       model3DLayout = nullptr;
    ID3D11Buffer*            model3DVB     = nullptr;

    // fallback white 1x1 texture + SRV (handle index 1 reserved)
    ID3D11Texture2D*         whiteTex      = nullptr;
    ID3D11ShaderResourceView* whiteSRV    = nullptr;

    // Drawing dimensions: the content rectangle every projection maps over
    // (= the back buffer in MARNI_ASPECT_STRETCH). Port-added split: bbW/bbH
    // are the real swap-chain size, cx/cy the content rectangle's origin.
    DWORD                    width         = 0;
    DWORD                    height        = 0;
    DWORD                    bbW           = 0;
    DWORD                    bbH           = 0;
    LONG                     cx            = 0;
    LONG                     cy            = 0;
    int                      aspectMode    = MARNI_ASPECT_STRETCH;
    int                      vpMode        = MARNI_VP_CONTENT;   // mod: 16:9 pan-and-scan
    float                    vpPan         = 0.0f;
    BOOL                     wideFrame     = FALSE;  // this frame drew a 16:9 scene (CRT rect)

    // The centred 16:9 box (held to 16:9 on wider buffers) and the 4:3
    // scene's height when scaled to its width.
    void WideBox(LONG* offX, LONG* effW, float* sceneH) const
    {
        LONG w = (LONG)bbW;
        LONG maxW = (LONG)(((long long)bbH * 16 + 8) / 9);
        if (w > maxW) w = maxW;
        *effW = w;
        *offX = ((LONG)bbW - w) / 2;
        *sceneH = (float)w * 0.75f;
    }
    BOOL                     ready         = FALSE;

    // Recompute the content rectangle from bbW/bbH and aspectMode.
    void UpdateContentRect()
    {
        int x = 0, y = 0, w = (int)bbW, h = (int)bbH;
        MarniComputeContentRect(aspectMode, (int)bbW, (int)bbH, &x, &y, &w, &h);
        cx = x; cy = y;
        width = (DWORD)w; height = (DWORD)h;
    }

    // Point the rasteriser at the content rectangle. The scissor matches it,
    // so nothing spills into the bars; Clear() still clears the whole target.
    void ApplyViewport()
    {
        if (!context) return;
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = (float)cx;
        vp.TopLeftY = (float)cy;
        vp.Width    = (float)width;
        vp.Height   = (float)height;
        vp.MaxDepth = 1.0f;
        D3D11_RECT sr = { cx, cy, cx + (LONG)width, cy + (LONG)height };
        // Mod: 16:9 pan-and-scan. Only when the buffer is wider than 4:3.
        // Wider than 16:9 (21:9) is held to a centred 16:9 box.
        LONG effW, offX; float sceneH;
        WideBox(&offX, &effW, &sceneH);
        if (aspectMode == MARNI_ASPECT_WIDE && vpMode != MARNI_VP_CONTENT &&
            sceneH > (float)bbH + 0.5f) {
            sr.left = offX; sr.top = 0; sr.right = offX + effW; sr.bottom = (LONG)bbH;
            vp.TopLeftX = (float)offX;
            vp.Width    = (float)effW;
            if (vpMode == MARNI_VP_SCENE) {
                vp.TopLeftY = -vpPan * sceneH / 240.0f;
                vp.Height   = sceneH;
            } else {
                vp.TopLeftY = 0.0f;
                vp.Height   = (float)bbH;
            }
        }
        context->RSSetViewports(1, &vp);
        context->RSSetScissorRects(1, &sr);
    }

    // texture handle table (index 0 reserved = NULL)
    TexSlot                  slots[MARNI_MAX_TEXTURES];

    // font cache (handle/dims only — D3D11 pointer stays in slots[])
    MarniHandle              fontHandle    = MARNI_NULL_HANDLE;
    int                      fontW         = 0;
    int                      fontH         = 0;

    //--- helpers ---------------------------------------------------------
    void ReleaseSlot(int idx);
    int  AllocSlot(ID3D11Texture2D* t, ID3D11ShaderResourceView* s,
                   int w, int h);
    void ReleaseAllState();
    void ReleaseAllTextures();

    // Port-added offscreen scene management.
    void ReleaseScene();
    void CreateScene();          // (re)build for bbW x bbH, smaa, crtShader
    void BindCurrent();          // OMSetRenderTargets on the current target
    ID3D11Texture2D* ResolvedScene();   // single-sample copy of the scene, or null
};

// ----------------------------------------------------------------------------
// Global singleton
// ----------------------------------------------------------------------------
static MarniDX* g_pDX = nullptr;

MarniDX* Marni_DX() { return g_pDX; }

MarniDX* MarniDX_Create()
{
    if (g_pDX) return g_pDX;
    g_pDX = new (std::nothrow) MarniDX();
    return g_pDX;
}

void MarniDX_DestroyGlobal()
{
    if (g_pDX) {
        g_pDX->Destroy();
        delete g_pDX;
        g_pDX = nullptr;
    }
}

MarniDX::MarniDX()  : m_pImpl(new MarniDX::Impl()) {}
MarniDX::~MarniDX() { delete m_pImpl; m_pImpl = nullptr; }

// ============================================================================
// Local helpers (file-local, all D3D11 stays in this TU)
// ============================================================================
static void BuildOrthoMatrix(float* m, float left, float right,
                             float bottom, float top)
{
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = 2.0f / (right - left);
    m[5]  = 2.0f / (top - bottom);
    m[10] = 1.0f;
    m[12] = (left + right) / (left - right);
    m[13] = (top + bottom) / (bottom - top);
    m[15] = 1.0f;
}

static bool CompileShaders(ID3D11Device* dev,
                           ID3D11VertexShader** outVS,
                           ID3D11PixelShader**  outPS,
                           ID3D11InputLayout**  outLayout)
{
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = D3DCompile(g_QuadVS_Source, strlen(g_QuadVS_Source),
        "QuadVS", nullptr, nullptr, "main", "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) { OutputDebugStringA("[MarniDX] VS compile: ");
            OutputDebugStringA((char*)errBlob->GetBufferPointer());
            OutputDebugStringA("\n"); errBlob->Release(); }
        return false;
    }

    hr = D3DCompile(g_QuadPS_Source, strlen(g_QuadPS_Source),
        "QuadPS", nullptr, nullptr, "main", "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) { OutputDebugStringA("[MarniDX] PS compile: ");
            OutputDebugStringA((char*)errBlob->GetBufferPointer());
            OutputDebugStringA("\n"); errBlob->Release(); }
        vsBlob->Release();
        return false;
    }

    hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), nullptr, outVS);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(), nullptr, outPS);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), outLayout);
    vsBlob->Release(); psBlob->Release();
    return SUCCEEDED(hr);
}

// Compile the depth-buffered model VS + its input layout. Reuses the quad PS,
// whose PS_INPUT signature is identical.
static bool CompileModel3DShader(ID3D11Device* dev,
                                 ID3D11VertexShader** outVS,
                                 ID3D11InputLayout**  outLayout)
{
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = D3DCompile(g_Model3DVS_Source, strlen(g_Model3DVS_Source),
        "Model3DVS", nullptr, nullptr, "main", "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) { OutputDebugStringA("[MarniDX] model VS compile: ");
            OutputDebugStringA((char*)errBlob->GetBufferPointer());
            OutputDebugStringA("\n"); errBlob->Release(); }
        return false;
    }

    hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), nullptr, outVS);
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), outLayout);
    vsBlob->Release();
    return SUCCEEDED(hr);
}

// Convert host bpp (4/8/16/24/32) to a contiguous RGBA8 (one DWORD/pixel)
// buffer suitable for DXGI_FORMAT_R8G8B8A8_UNORM upload.
// Returns a heap-allocated array (caller frees via free) or nullptr.
static DWORD* ConvertToRGBA8(int width, int height, int bpp,
                             const void* pixelData)
{
    if (!pixelData || width <= 0 || height <= 0) return nullptr;
    DWORD total = (DWORD)width * (DWORD)height;
    DWORD* rgba = (DWORD*)malloc(sizeof(DWORD) * total);
    if (!rgba) return nullptr;

    if (bpp == 32) {
        memcpy(rgba, pixelData, sizeof(DWORD) * total);
    } else if (bpp == 24) {
        BYTE* s = (BYTE*)pixelData;
        for (DWORD i = 0; i < total; i++) {
            rgba[i] = 0xFF000000u | (s[0]) | (s[1] << 8) | (s[2] << 16);
            s += 3;
        }
    } else if (bpp == 16) {
        WORD* s = (WORD*)pixelData;
        for (DWORD i = 0; i < total; i++) {
            WORD p = s[i];
            DWORD a = (p & 0x8000) ? 0xFF : 0x00;
            DWORD r = ((p >> 10) & 0x1F) * 255 / 31;
            DWORD g = ((p >> 5)  & 0x1F) * 255 / 31;
            DWORD b = ( p        & 0x1F) * 255 / 31;
            rgba[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    } else if (bpp == 8) {
        BYTE* s = (BYTE*)pixelData;
        for (DWORD i = 0; i < total; i++) {
            DWORD c = s[i];
            rgba[i] = 0xFF000000u | (c << 16) | (c << 8) | c;
        }
    } else if (bpp == 4) {
        WORD* s = (WORD*)pixelData;
        int words = total / 4;
        for (int i = 0; i < words; i++) {
            WORD w = s[i];
            DWORD p0 = ((w >> 0)  & 0xF) * 17;
            DWORD p1 = ((w >> 4)  & 0xF) * 17;
            DWORD p2 = ((w >> 8)  & 0xF) * 17;
            DWORD p3 = ((w >> 12) & 0xF) * 17;
            int b = i * 4;
            rgba[b]     = 0xFF000000u | (p0 << 16) | (p0 << 8) | p0;
            rgba[b + 1] = 0xFF000000u | (p1 << 16) | (p1 << 8) | p1;
            rgba[b + 2] = 0xFF000000u | (p2 << 16) | (p2 << 8) | p2;
            rgba[b + 3] = 0xFF000000u | (p3 << 16) | (p3 << 8) | p3;
        }
    } else {
        free(rgba);
        return nullptr;
    }
    return rgba;
}

// ----------------------------------------------------------------------------
// Impl helpers
// ----------------------------------------------------------------------------
void MarniDX::Impl::ReleaseSlot(int idx)
{
    if (idx <= 0 || idx >= MARNI_MAX_TEXTURES) return;
    TexSlot& t = slots[idx];
    if (t.srv) { t.srv->Release();   t.srv = nullptr; }
    if (t.tex) { t.tex->Release();   t.tex = nullptr; }
    t.width = 0; t.height = 0;
}

int MarniDX::Impl::AllocSlot(ID3D11Texture2D* t, ID3D11ShaderResourceView* s,
                             int w, int h)
{
    if (!t || !s) {
        if (t) t->Release();
        if (s) s->Release();
        return 0;
    }
    for (int i = 1; i < MARNI_MAX_TEXTURES; i++) {
        if (slots[i].tex == nullptr) {
            slots[i].tex   = t;
            slots[i].srv   = s;
            slots[i].width = w;
            slots[i].height = h;
            return i;
        }
    }
    // table full — reject.
    t->Release(); s->Release();
    return 0;
}

void MarniDX::Impl::ReleaseAllTextures()
{
    for (int i = 1; i < MARNI_MAX_TEXTURES; i++) ReleaseSlot(i);
    // white texture lives in slot? It's tracked by whiteTex/whiteSRV not slot.
    if (whiteSRV) { whiteSRV->Release(); whiteSRV = nullptr; }
    if (whiteTex) { whiteTex->Release(); whiteTex = nullptr; }
}

void MarniDX::Impl::ReleaseAllState()
{
    if (context) context->ClearState();
    if (quadLayout)    { quadLayout->Release();    quadLayout    = nullptr; }
    if (quadVS)        { quadVS->Release();        quadVS        = nullptr; }
    if (quadPS)        { quadPS->Release();        quadPS        = nullptr; }
    if (quadVB)        { quadVB->Release();        quadVB        = nullptr; }
    if (spriteCB)      { spriteCB->Release();      spriteCB      = nullptr; }
    if (rasterScissor) { rasterScissor->Release(); rasterScissor = nullptr; }
    if (blendAlpha)    { blendAlpha->Release();    blendAlpha    = nullptr; }
    if (blendAdd)      { blendAdd->Release();      blendAdd      = nullptr; }
    if (blendDisabled) { blendDisabled->Release(); blendDisabled = nullptr; }
    if (sampLinear)    { sampLinear->Release();    sampLinear    = nullptr; }
    if (sampPoint)     { sampPoint->Release();     sampPoint     = nullptr; }
    if (model3DLayout) { model3DLayout->Release(); model3DLayout = nullptr; }
    if (model3DVS)     { model3DVS->Release();     model3DVS     = nullptr; }
    if (model3DVB)     { model3DVB->Release();     model3DVB     = nullptr; }
    if (depthEnabled)  { depthEnabled->Release();  depthEnabled  = nullptr; }
    if (depthDisabled) { depthDisabled->Release(); depthDisabled = nullptr; }
    if (depthTestNoWrite) { depthTestNoWrite->Release(); depthTestNoWrite = nullptr; }
    ReleaseScene();
    if (postVS)          { postVS->Release();          postVS          = nullptr; }
    if (copyPS)          { copyPS->Release();          copyPS          = nullptr; }
    if (crtPS)           { crtPS->Release();           crtPS           = nullptr; }
    if (crtCB)           { crtCB->Release();           crtCB           = nullptr; }
    if (smaaEdgeVS)      { smaaEdgeVS->Release();      smaaEdgeVS      = nullptr; }
    if (smaaEdgePS)      { smaaEdgePS->Release();      smaaEdgePS      = nullptr; }
    if (smaaWeightVS)    { smaaWeightVS->Release();    smaaWeightVS    = nullptr; }
    if (smaaWeightPS)    { smaaWeightPS->Release();    smaaWeightPS    = nullptr; }
    if (smaaBlendVS)     { smaaBlendVS->Release();     smaaBlendVS     = nullptr; }
    if (smaaBlendPS)     { smaaBlendPS->Release();     smaaBlendPS     = nullptr; }
    if (smaaCB)          { smaaCB->Release();          smaaCB          = nullptr; }
    if (smaaAreaSrv)     { smaaAreaSrv->Release();     smaaAreaSrv     = nullptr; }
    if (smaaAreaTex)     { smaaAreaTex->Release();     smaaAreaTex     = nullptr; }
    if (smaaSearchSrv)   { smaaSearchSrv->Release();   smaaSearchSrv   = nullptr; }
    if (smaaSearchTex)   { smaaSearchTex->Release();   smaaSearchTex   = nullptr; }
    if (rasterNoScissor) { rasterNoScissor->Release(); rasterNoScissor = nullptr; }
    curRtv = nullptr; curDsv = nullptr;
    if (depthStencilView){ depthStencilView->Release(); depthStencilView = nullptr; }
    if (depthStencil)    { depthStencil->Release();     depthStencil     = nullptr; }
    if (rtv)            { rtv->Release();              rtv              = nullptr; }
    if (context)        { context->Release();          context          = nullptr; }
    if (swapChain)      { swapChain->Release();        swapChain        = nullptr; }
    if (device)         { device->Release();           device           = nullptr; }
}

// ============================================================================
// Port-added offscreen scene (SMAA / CRT shader)
// ============================================================================
static bool CompileBlob(const char* src, const char* name, const char* target,
                        ID3DBlob** out, const char* entry = "main")
{
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, entry,
                            target, D3DCOMPILE_ENABLE_STRICTNESS, 0, out, &err);
    if (FAILED(hr)) {
        OutputDebugStringA("[MarniDX] ");
        OutputDebugStringA(name);
        OutputDebugStringA(" compile failed: ");
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        OutputDebugStringA("\n");
    }
    if (err) err->Release();
    return SUCCEEDED(hr);
}

// One immutable lookup texture for SMAA (AreaTex RG8, SearchTex R8).
static void CreateSmaaLookup(ID3D11Device* device, const unsigned char* bytes,
                             UINT w, UINT h, UINT pitch, DXGI_FORMAT fmt,
                             ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = bytes;
    sd.SysMemPitch = pitch;
    if (SUCCEEDED(device->CreateTexture2D(&td, &sd, tex)) &&
        FAILED(device->CreateShaderResourceView(*tex, nullptr, srv))) {
        (*tex)->Release();
        *tex = nullptr;
    }
}

// SMAA's six shaders, its constant buffer and lookup textures. Any failure
// leaves SmaaReady() false and anti-aliasing simply unavailable.
static void CompileSmaaShaders(MarniDX::Impl* p)
{
    std::string src = g_SmaaPrefix_Source;
    src += g_SmaaHlsl;
    src += g_SmaaSuffix_Source;
    ID3DBlob* b = nullptr;
    struct { const char* entry; bool vs; void** out; } passes[] = {
        { "SmaaEdgeVS",   true,  (void**)&p->smaaEdgeVS   },
        { "SmaaEdgePS",   false, (void**)&p->smaaEdgePS   },
        { "SmaaWeightVS", true,  (void**)&p->smaaWeightVS },
        { "SmaaWeightPS", false, (void**)&p->smaaWeightPS },
        { "SmaaBlendVS",  true,  (void**)&p->smaaBlendVS  },
        { "SmaaBlendPS",  false, (void**)&p->smaaBlendPS  },
    };
    for (auto& ps : passes) {
        if (!CompileBlob(src.c_str(), ps.entry, ps.vs ? "vs_4_0" : "ps_4_0", &b, ps.entry))
            continue;
        if (ps.vs)
            p->device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
                                          (ID3D11VertexShader**)ps.out);
        else
            p->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
                                         (ID3D11PixelShader**)ps.out);
        b->Release();
    }
    D3D11_BUFFER_DESC cb = {};
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.ByteWidth      = sizeof(SmaaConstantBuffer);
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    p->device->CreateBuffer(&cb, nullptr, &p->smaaCB);
    CreateSmaaLookup(p->device, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT,
                     AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM, &p->smaaAreaTex, &p->smaaAreaSrv);
    CreateSmaaLookup(p->device, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                     SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM, &p->smaaSearchTex, &p->smaaSearchSrv);
    if (!p->SmaaReady()) crashlog_mark("video: SMAA unavailable (shader or texture setup failed)");
}

// The post shaders are optional: if any of them fails, SMAA and the CRT
// shader simply stay unavailable and the game draws straight to the back
// buffer as before.
static void CompilePostShaders(MarniDX::Impl* p)
{
    ID3DBlob* b = nullptr;
    if (CompileBlob(g_PostVS_Source, "PostVS", "vs_4_0", &b)) {
        p->device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p->postVS);
        b->Release();
    }
    if (CompileBlob(g_CopyPS_Source, "CopyPS", "ps_4_0", &b)) {
        p->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p->copyPS);
        b->Release();
    }
    if (CompileBlob(g_CrtPS_Source, "CrtPS", "ps_4_0", &b)) {
        p->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p->crtPS);
        b->Release();
    }
    D3D11_BUFFER_DESC cb = {};
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.ByteWidth      = sizeof(CrtConstantBuffer);
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    p->device->CreateBuffer(&cb, nullptr, &p->crtCB);

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode        = D3D11_FILL_SOLID;
    rs.CullMode        = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    p->device->CreateRasterizerState(&rs, &p->rasterNoScissor);
    CompileSmaaShaders(p);
}

void MarniDX::Impl::ReleaseScene()
{
    if (context) {
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    if (smaaOutSrv)   { smaaOutSrv->Release();   smaaOutSrv   = nullptr; }
    if (smaaOutRtv)   { smaaOutRtv->Release();   smaaOutRtv   = nullptr; }
    if (smaaOutTex)   { smaaOutTex->Release();   smaaOutTex   = nullptr; }
    if (smaaBlendSrv) { smaaBlendSrv->Release(); smaaBlendSrv = nullptr; }
    if (smaaBlendRtv) { smaaBlendRtv->Release(); smaaBlendRtv = nullptr; }
    if (smaaBlendTex) { smaaBlendTex->Release(); smaaBlendTex = nullptr; }
    if (smaaEdgesSrv) { smaaEdgesSrv->Release(); smaaEdgesSrv = nullptr; }
    if (smaaEdgesRtv) { smaaEdgesRtv->Release(); smaaEdgesRtv = nullptr; }
    if (smaaEdgesTex) { smaaEdgesTex->Release(); smaaEdgesTex = nullptr; }
    if (sceneDsv)   { sceneDsv->Release();   sceneDsv   = nullptr; }
    if (sceneDepth) { sceneDepth->Release(); sceneDepth = nullptr; }
    if (sceneSrv)   { sceneSrv->Release();   sceneSrv   = nullptr; }
    if (sceneRtv)   { sceneRtv->Release();   sceneRtv   = nullptr; }
    if (sceneTex)   { sceneTex->Release();   sceneTex   = nullptr; }
    smaaActive = FALSE;
    curRtv = rtv;
    curDsv = depthStencilView;
}

void MarniDX::Impl::CreateScene()
{
    {
        char m[128];
        sprintf(m, "video: scene rebuild %ux%u smaa=%d crt=%d post=%d%d%d%d",
                (unsigned)bbW, (unsigned)bbH, (int)smaa, (int)crtShader,
                postVS != nullptr, copyPS != nullptr, crtPS != nullptr, rasterNoScissor != nullptr);
        crashlog_mark(m);
    }
    ReleaseScene();
    if (!device || !UsesScene() || bbW == 0 || bbH == 0 ||
        !postVS || !copyPS || !rasterNoScissor) {
        BindCurrent();
        return;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = bbW; td.Height = bbH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &sceneTex)) &&
              SUCCEEDED(device->CreateRenderTargetView(sceneTex, nullptr, &sceneRtv)) &&
              SUCCEEDED(device->CreateShaderResourceView(sceneTex, nullptr, &sceneSrv));
    if (ok) {
        D3D11_TEXTURE2D_DESC dd = td;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ok = SUCCEEDED(device->CreateTexture2D(&dd, nullptr, &sceneDepth)) &&
             SUCCEEDED(device->CreateDepthStencilView(sceneDepth, nullptr, &sceneDsv));
    }
    if (!ok) {
        OutputDebugStringA("[MarniDX] offscreen scene creation failed - drawing to the back buffer\n");
        crashlog_mark("video: scene creation FAILED - drawing to the back buffer");
        ReleaseScene();
        BindCurrent();
        return;
    }
    // SMAA's targets. Without them the scene still works; anti-aliasing is
    // just skipped.
    if (smaa && SmaaReady()) {
        auto target = [&](DXGI_FORMAT fmt, ID3D11Texture2D** t,
                          ID3D11RenderTargetView** rv, ID3D11ShaderResourceView** sv) {
            D3D11_TEXTURE2D_DESC d = td;
            d.Format = fmt;
            return SUCCEEDED(device->CreateTexture2D(&d, nullptr, t)) &&
                   SUCCEEDED(device->CreateRenderTargetView(*t, nullptr, rv)) &&
                   SUCCEEDED(device->CreateShaderResourceView(*t, nullptr, sv));
        };
        smaaActive =
            target(DXGI_FORMAT_R8G8_UNORM, &smaaEdgesTex, &smaaEdgesRtv, &smaaEdgesSrv) &&
            target(DXGI_FORMAT_R8G8B8A8_UNORM, &smaaBlendTex, &smaaBlendRtv, &smaaBlendSrv) &&
            (!crtShader ||
             target(DXGI_FORMAT_R8G8B8A8_UNORM, &smaaOutTex, &smaaOutRtv, &smaaOutSrv));
        if (!smaaActive) crashlog_mark("video: SMAA target creation failed - SMAA off");
    }
    curRtv = sceneRtv;
    curDsv = sceneDsv;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(sceneRtv, black);
    context->ClearDepthStencilView(sceneDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    BindCurrent();
    char msg[96];
    sprintf(msg, "[MarniDX] scene %ux%u, SMAA %s, CRT %s\n",
            (unsigned)bbW, (unsigned)bbH, smaaActive ? "on" : "off", crtShader ? "on" : "off");
    OutputDebugStringA(msg);
    crashlog_mark(msg);
    s_postTrace = 3;
}

void MarniDX::Impl::BindCurrent()
{
    if (!context) return;
    if (curRtv) context->OMSetRenderTargets(1, &curRtv, curDsv);
    ApplyViewport();
    if (rasterScissor) context->RSSetState(rasterScissor);
}

ID3D11Texture2D* MarniDX::Impl::ResolvedScene()
{
    return sceneTex;
}

// ============================================================================
// Lifecycle
// ============================================================================
BOOL MarniDX::Create(HWND hWnd, int width, int height, BOOL fullScreen,
                     int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (!p) return FALSE;

    if (width  < 320) width  = 640;
    if (height < 240) height = 480;

    // Borderless-fullscreen presentation: NEVER create a DXGI exclusive
    // swap chain. Exclusive scanout bypasses DWM composition, which made
    // the GDI-drawn MCI FMV frames invisible (audio-only playback), its
    // asynchronous mode switches exposed the desktop behind resizing
    // windows after each FMV, and releasing an exclusive swap chain could
    // hang the process at exit leaving the panel black. A windowed
    // blt-model swap chain presented into a screen-sized WS_POPUP window
    // gives the same full-screen result without any of those problems;
    // DWM stretches the game-resolution back buffer over the monitor.
    if (fullScreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            // Done BEFORE the device exists so the resulting WM_SIZE lands
            // while ready==FALSE and cannot trigger a swap-chain resize.
            SetWindowPos(hWnd, HWND_TOPMOST,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    DXGI_SWAP_CHAIN_DESC sc = {};
    sc.BufferCount        = 2;
    sc.BufferDesc.Width   = width;
    sc.BufferDesc.Height  = height;
    sc.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferDesc.RefreshRate.Numerator   = 60;
    sc.BufferDesc.RefreshRate.Denominator = 1;
    sc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow       = hWnd;
    sc.SampleDesc.Count   = 1;
    sc.Windowed           = TRUE;
    sc.Flags              = 0;

    D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL got;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
        D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 3, D3D11_SDK_VERSION,
        &sc, &p->swapChain, &p->device, &got, &p->context);
    if (FAILED(hr)) {
        OutputDebugStringA("[MarniDX] Hardware D3D11 failed, trying WARP\n");
        // Drop any partially-created device before retrying.
        if (p->device)    { p->device->Release();    p->device    = nullptr; }
        if (p->context)   { p->context->Release();   p->context   = nullptr; }
        if (p->swapChain) { p->swapChain->Release(); p->swapChain = nullptr; }
        hr = D3D11CreateDeviceAndSwapChain(nullptr,
            D3D_DRIVER_TYPE_WARP, nullptr, 0, fls, 3, D3D11_SDK_VERSION,
            &sc, &p->swapChain, &p->device, &got, &p->context);
    }
    if (FAILED(hr)) {
        OutputDebugStringA("[MarniDX] D3D11 device creation FAILED\n");
        return FALSE;
    }
    OutputDebugStringA("[MarniDX] D3D11 device created OK\n");
    p->hWnd   = hWnd;
    p->bbW    = (DWORD)width;
    p->bbH    = (DWORD)height;
    p->UpdateContentRect();

    // back buffer -> RTV
    ID3D11Texture2D* bb = nullptr;
    hr = p->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (SUCCEEDED(hr)) {
        hr = p->device->CreateRenderTargetView(bb, nullptr, &p->rtv);
        bb->Release();
    }
    if (FAILED(hr)) { p->ReleaseAllState(); return FALSE; }

    // depth/stencil
    D3D11_TEXTURE2D_DESC ds = {};
    ds.Width            = width;  ds.Height = height;
    ds.MipLevels        = 1;      ds.ArraySize = 1;
    ds.Format           = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds.SampleDesc.Count = 1;
    ds.Usage            = D3D11_USAGE_DEFAULT;
    ds.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(p->device->CreateTexture2D(&ds, nullptr, &p->depthStencil)))
        p->device->CreateDepthStencilView(p->depthStencil, nullptr,
                                          &p->depthStencilView);

    // rasterizer (scissor on)
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode         = D3D11_FILL_SOLID;
    rs.CullMode         = D3D11_CULL_NONE;
    rs.ScissorEnable    = TRUE;
    rs.DepthClipEnable  = TRUE;
    if (FAILED(p->device->CreateRasterizerState(&rs, &p->rasterScissor))) {
        p->ReleaseAllState(); return FALSE;
    }

    // alpha blend
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable        = TRUE;
    bd.RenderTarget[0].SrcBlend           = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend          = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp            = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha      = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha     = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha       = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(p->device->CreateBlendState(&bd, &p->blendAlpha))) {
        p->ReleaseAllState(); return FALSE;
    }

    // additive blend (used by MARNI_BLEND_ADD)
    bd.RenderTarget[0].SrcBlend  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    p->device->CreateBlendState(&bd, &p->blendAdd);

    // disabled blend (straight overwrite)
    D3D11_BLEND_DESC bdOff = {};
    bdOff.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    p->device->CreateBlendState(&bdOff, &p->blendDisabled);

    // samplers
    D3D11_SAMPLER_DESC ss = {};
    ss.Filter       = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ss.MaxLOD        = D3D11_FLOAT32_MAX;
    if (FAILED(p->device->CreateSamplerState(&ss, &p->sampLinear))) {
        p->ReleaseAllState(); return FALSE;
    }
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(p->device->CreateSamplerState(&ss, &p->sampPoint))) {
        p->ReleaseAllState(); return FALSE;
    }

    // depth disabled (2D)
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable    = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc      = D3D11_COMPARISON_ALWAYS;
    if (FAILED(p->device->CreateDepthStencilState(&dd, &p->depthDisabled))) {
        p->ReleaseAllState(); return FALSE;
    }

    // depth enabled (3D models) - the 2D layers neither test nor write, so
    // enabling it here only affects the TMD path.
    D3D11_DEPTH_STENCIL_DESC de = {};
    de.DepthEnable    = TRUE;
    de.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    de.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(p->device->CreateDepthStencilState(&de, &p->depthEnabled))) {
        p->ReleaseAllState(); return FALSE;
    }

    // depth test WITHOUT write - translucent world quads (the ground shadow /
    // blood pool fade polys). They must clip against the model geometry the
    // way the original's Z-buffered viewport quads did, but as alpha-blended
    // primitives they must not poison the buffer for the painter-ordered
    // draws around them.
    D3D11_DEPTH_STENCIL_DESC dt = {};
    dt.DepthEnable    = TRUE;
    dt.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dt.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(p->device->CreateDepthStencilState(&dt, &p->depthTestNoWrite))) {
        p->ReleaseAllState(); return FALSE;
    }

    // shaders
    if (!CompileShaders(p->device, &p->quadVS, &p->quadPS, &p->quadLayout)) {
        OutputDebugStringA("[MarniDX] shader compile failed\n");
        p->ReleaseAllState(); return FALSE;
    }
    if (!CompileModel3DShader(p->device, &p->model3DVS, &p->model3DLayout)) {
        OutputDebugStringA("[MarniDX] model shader compile failed\n");
        p->ReleaseAllState(); return FALSE;
    }

    // vertex buffer (6 verts/sprite * 512 sprites)
    D3D11_BUFFER_DESC vb = {};
    vb.Usage          = D3D11_USAGE_DYNAMIC;
    vb.ByteWidth      = sizeof(QuadVertex) * 6 * 512;
    vb.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(p->device->CreateBuffer(&vb, nullptr, &p->quadVB))) {
        p->ReleaseAllState(); return FALSE;
    }

    // 3D model vertex buffer (3 verts/tri * 1024 triangles per call)
    D3D11_BUFFER_DESC mvb = {};
    mvb.Usage          = D3D11_USAGE_DYNAMIC;
    mvb.ByteWidth      = sizeof(Model3DVertex) * 3 * 1024;
    mvb.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    mvb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(p->device->CreateBuffer(&mvb, nullptr, &p->model3DVB))) {
        p->ReleaseAllState(); return FALSE;
    }

    // sprite constant buffer
    D3D11_BUFFER_DESC cb = {};
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.ByteWidth      = sizeof(SpriteConstantBuffer);
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(p->device->CreateBuffer(&cb, nullptr, &p->spriteCB))) {
        p->ReleaseAllState(); return FALSE;
    }

    // white 1x1 texture
    D3D11_TEXTURE2D_DESC wt = {};
    wt.Width    = 1; wt.Height = 1; wt.MipLevels = 1; wt.ArraySize = 1;
    wt.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    wt.SampleDesc.Count = 1;
    wt.Usage    = D3D11_USAGE_IMMUTABLE;
    wt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    DWORD whitePixel = 0xFFFFFFFFu;
    D3D11_SUBRESOURCE_DATA wi = {};
    wi.pSysMem     = &whitePixel;
    wi.SysMemPitch = 4;
    if (SUCCEEDED(p->device->CreateTexture2D(&wt, &wi, &p->whiteTex))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC wsd = {};
        wsd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        wsd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        wsd.Texture2D.MipLevels = 1;
        p->device->CreateShaderResourceView(p->whiteTex, &wsd, &p->whiteSRV);
    }

    // Port-added: SMAA / CRT post-processing (optional - see CompilePostShaders).
    CompilePostShaders(p);

    // bind RTV + set viewport/scissor (the offscreen scene when SMAA or the
    // CRT shader is on, otherwise the back buffer)
    p->CreateScene();

    p->ready = TRUE;

    // In borderless fullscreen, render at the monitor's native resolution.
    // The game-space -> backbuffer scale (MarniGetRenderScale = physical/
    // logical) is applied at draw time with POINT sampling, so a native-
    // resolution backbuffer keeps the 2D art crisp. Leaving the backbuffer
    // at the game resolution would make DWM stretch it over the screen-
    // sized window with bilinear filtering, blurring every glyph.
    if (fullScreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            int monW = mi.rcMonitor.right - mi.rcMonitor.left;
            int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (monW >= 320 && monH >= 240 &&
                (monW != (int)p->bbW || monH != (int)p->bbH)) {
                ChangeDisplayMode((DWORD)monW, (DWORD)monH, FALSE);
            }
        }
    }

    if (outWidth)  *outWidth  = (int)p->width;
    if (outHeight) *outHeight = (int)p->height;
    return TRUE;
}

// ----------------------------------------------------------------------------
// Local helpers (file-local, all D3D11 stays in this TU)
// =============================================================================

void MarniDX::Destroy()
{
    if (!m_pImpl) return;
    m_pImpl->ReleaseAllTextures();
    m_pImpl->ReleaseAllState();
    m_pImpl->ready = FALSE;
    m_pImpl->fontHandle = MARNI_NULL_HANDLE;
    m_pImpl->fontW = 0;
    m_pImpl->fontH = 0;
}


BOOL MarniDX::IsReady() const { return m_pImpl && m_pImpl->ready; }

void MarniDX::GetBackBufferSize(DWORD* w, DWORD* h) const
{
    if (w) *w = m_pImpl ? m_pImpl->width  : 0;
    if (h) *h = m_pImpl ? m_pImpl->height : 0;
}

int MarniDX::ChangeDisplayMode(DWORD newW, DWORD newH, BOOL fullScreen)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready || !p->swapChain || !p->context) return 0;
    if (newW == 0 || newH == 0) return 0;
    if (newW < 320) newW = 320;
    if (newH < 240) newH = 240;

    p->ReleaseScene();
    p->context->OMSetRenderTargets(0, nullptr, nullptr);
    p->curRtv = nullptr; p->curDsv = nullptr;
    if (p->rtv) { p->rtv->Release(); p->rtv = nullptr; }
    // The depth surface has to be resized with the render target: D3D11 rejects
    // a depth view whose dimensions differ from the colour view, which would
    // silently leave the 3D models with no depth buffer after a resize.
    if (p->depthStencilView) { p->depthStencilView->Release(); p->depthStencilView = nullptr; }
    if (p->depthStencil)     { p->depthStencil->Release();     p->depthStencil     = nullptr; }

    HRESULT hr = p->swapChain->ResizeBuffers(2, newW, newH,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) return 0;

    p->bbW = newW; p->bbH = newH;
    p->UpdateContentRect();
    ID3D11Texture2D* bb = nullptr;
    hr = p->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (SUCCEEDED(hr)) {
        p->device->CreateRenderTargetView(bb, nullptr, &p->rtv);
        bb->Release();
    }

    D3D11_TEXTURE2D_DESC ds = {};
    ds.Width            = newW;  ds.Height = newH;
    ds.MipLevels        = 1;     ds.ArraySize = 1;
    ds.Format           = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds.SampleDesc.Count = 1;
    ds.Usage            = D3D11_USAGE_DEFAULT;
    ds.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(p->device->CreateTexture2D(&ds, nullptr, &p->depthStencil)))
        p->device->CreateDepthStencilView(p->depthStencil, nullptr,
                                          &p->depthStencilView);

    p->curRtv = p->rtv;
    p->curDsv = p->depthStencilView;
    p->CreateScene();       // rebuilds the scene at the new size and binds
    return 1;
}

void MarniDX::SetAspectMode(int mode)
{
    Impl* p = m_pImpl;
    if (!p) return;
    if (mode < 0 || mode >= MARNI_ASPECT_COUNT) mode = MARNI_ASPECT_STRETCH;
    p->aspectMode = mode;
    if (p->bbW == 0 || p->bbH == 0) return;   // before Create: applied there
    p->UpdateContentRect();
    p->ApplyViewport();
}

void MarniDX::SetViewportMode(int mode, float panLines)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready) return;
    if (p->vpMode == mode && p->vpPan == panLines) return;
    p->vpMode = mode;
    p->vpPan  = panLines;
    if (mode != MARNI_VP_CONTENT && p->aspectMode == MARNI_ASPECT_WIDE) {
        LONG ox, ew; float sh;
        p->WideBox(&ox, &ew, &sh);
        if (sh > (float)p->bbH + 0.5f) p->wideFrame = TRUE;
    }
    p->ApplyViewport();
}

float MarniDX::WideVisibleLines() const
{
    Impl* p = m_pImpl;
    if (!p || p->bbW == 0) return 240.0f;
    float effW = (float)p->bbW;
    const float maxW = (float)p->bbH * 16.0f / 9.0f;
    if (effW > maxW) effW = maxW;
    float v = 240.0f * (float)p->bbH / (effW * 0.75f);
    return v > 240.0f ? 240.0f : v;
}

int MarniDX::GetAspectMode() const
{
    return m_pImpl ? m_pImpl->aspectMode : MARNI_ASPECT_STRETCH;
}

int MarniDX::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready) return 1;
    switch (msg) {
    case WM_SIZE:
        if (!p->swapChain || LOWORD(lParam) == 0) return 1;
        ChangeDisplayMode(LOWORD(lParam), HIWORD(lParam), FALSE);
        return 1;
    default:
        return 1;
    }
    (void)hwnd; (void)wParam;
}

unsigned int MarniDX::QueryVideoMemory(BOOL softwareRenderer) const
{
    if (softwareRenderer) return 0x75bcd15u;
    Impl* p = m_pImpl;
    if (!p || !p->device) return 128u * 1024u * 1024u;

    IDXGIDevice* dxDev = nullptr;
    if (SUCCEEDED(p->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxDev))) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dxDev->GetAdapter(&ad))) {
            DXGI_ADAPTER_DESC d;
            HRESULT hr = ad->GetDesc(&d);
            ad->Release();
            dxDev->Release();
            if (SUCCEEDED(hr)) return (unsigned int)(d.DedicatedVideoMemory & 0x7FFFFFFFu);
        } else { dxDev->Release(); }
    }
    return 128u * 1024u * 1024u;
}

// ============================================================================
// Display mode / adapter enumeration (DXGI remains inside this TU)
// ============================================================================
void MarniDX::EnumerateDisplayModes(DisplayModeInfo* outModes, int maxModes,
                                    int* outCount)
{
    Impl* p = m_pImpl;
    int count = 0;
    if (p && p->device && outModes && maxModes > 0) {
        IDXGIDevice* dxDev = nullptr;
        if (SUCCEEDED(p->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxDev))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxDev->GetAdapter(&ad))) {
                IDXGIOutput* out = nullptr;
                if (SUCCEEDED(ad->EnumOutputs(0, &out))) {
                    UINT num = 0;
                    out->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num, nullptr);
                    if (num > 0) {
                        DXGI_MODE_DESC* modes = new DXGI_MODE_DESC[num];
                        out->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num, modes);
                        for (UINT i = 0; i < num && count < maxModes; i++) {
                            bool dup = false;
                            for (int j = 0; j < count; j++)
                                if (outModes[j].dwWidth  == modes[i].Width &&
                                    outModes[j].dwHeight == modes[i].Height)
                                { dup = true; break; }
                            if (dup) continue;
                            outModes[count].dwWidth  = modes[i].Width;
                            outModes[count].dwHeight = modes[i].Height;
                            outModes[count].dwBPP    = 32;
                            outModes[count].dwRefreshRate =
                                modes[i].RefreshRate.Numerator /
                                (modes[i].RefreshRate.Denominator ? modes[i].RefreshRate.Denominator : 1);
                            outModes[count].dwFlags = 1; // fullscreen
                            count++;
                        }
                        delete[] modes;
                    }
                    out->Release();
                }
                ad->Release();
            }
            dxDev->Release();
        }
    }
    if (count == 0) {
        // fallback defaults
        const DWORD defs[5][3] = {
            { 640, 480, 0 },  { 800, 600, 1 },  { 1024, 768, 1 },
            { 1280, 720, 1 }, { 1920, 1080, 1 },
        };
        for (int i = 0; i < 5 && i < maxModes; i++) {
            outModes[i].dwWidth = defs[i][0];
            outModes[i].dwHeight = defs[i][1];
            outModes[i].dwBPP    = 32;
            outModes[i].dwRefreshRate = 60;
            outModes[i].dwFlags  = defs[i][2];
            count++;
        }
    }
    if (outCount) *outCount = count;
}

void MarniDX::EnumerateAdapters(D3DRendererInfo* out, int max, int* outCount)
{
    int count = 0;
    IDXGIFactory* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&fac))) {
        IDXGIAdapter* ad = nullptr;
        for (UINT i = 0; fac->EnumAdapters(i, &ad) != DXGI_ERROR_NOT_FOUND
                          && count < max; i++) {
            DXGI_ADAPTER_DESC d;
            if (SUCCEEDED(ad->GetDesc(&d)) && out) {
                WideCharToMultiByte(CP_ACP, 0, d.Description, -1,
                    out[count].name, 256, nullptr, nullptr);
                out[count].flags = 1;
                count++;
            }
            ad->Release();
        }
        fac->Release();
    }
    if (count == 0 && out && max > 0) {
        strcpy_s(out[0].name, 256, "Primary Display Adapter");
        out[0].flags = 1;
        count = 1;
    }
    if (outCount) *outCount = count;
}

// ============================================================================
// Per-frame
// ============================================================================
void MarniDX::Clear(float r, float g, float b, float a)
{
    Impl* p = m_pImpl;
    if (!p || !p->context || !p->curRtv) return;
    float c[4] = { r, g, b, a };
    p->context->ClearRenderTargetView(p->curRtv, c);
    if (p->curDsv)
        p->context->ClearDepthStencilView(p->curDsv,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void MarniDX::Present()
{
    Impl* p = m_pImpl;
    if (!p || !p->swapChain) return;

    // Do NOT wait on a vblank by default. This engine paces itself in software:
    // the pump limits main_loop to one call per 33 ms (g_dwFrameIntervalMs) and
    // FrameRateGovernor decides which of those frames get presented. A blocking
    // Present puts the display in charge of both instead, and the two fight:
    // with SyncInterval 1 on a 60 Hz panel the real frame period becomes 33.33
    // ms, which the governor measures as 34 often enough to push its target to
    // 101 - inside the 101..103 notch the 104/132 scaling does not cover - which
    // sets g_bFrameSkipDetected, which DISABLES the 33 ms limiter, which lets a
    // burst of frames run at the full 16.7 ms refresh before the average settles
    // and pacing resumes. Measured: 31.6 ms per tick instead of 33, i.e. ~4%
    // fast, oscillating. The original blitted to the window without waiting for
    // a vblank, so its limiter always won.
    //
    // VSync is now also a Video Options setting; that note is why it stays
    // off by default.

    // Port-added: put the offscreen scene on the back buffer - through SMAA
    // when it is on, then a straight copy or the CRT shader (SMAA without the
    // CRT shader writes the back buffer itself).
    if (p->sceneTex && p->rtv) {
        if (s_postTrace > 0) crashlog_mark("video: post begin");
        ID3D11ShaderResourceView* srv = p->sceneSrv;
        const bool crt = p->crtShader && p->crtPS && p->crtCB;

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)p->bbW; vp.Height = (float)p->bbH; vp.MaxDepth = 1.0f;
        p->context->RSSetViewports(1, &vp);
        p->context->RSSetState(p->rasterNoScissor);
        p->context->IASetInputLayout(nullptr);
        p->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        float bf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        p->context->OMSetBlendState(p->blendDisabled, bf, 0xFFFFFFFFu);
        p->context->OMSetDepthStencilState(p->depthDisabled, 0);

        if (p->smaaActive) {
            SmaaConstantBuffer scb = {};
            scb.metrics[0] = 1.0f / (float)p->bbW; scb.metrics[1] = 1.0f / (float)p->bbH;
            scb.metrics[2] = (float)p->bbW;        scb.metrics[3] = (float)p->bbH;
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(p->context->Map(p->smaaCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &scb, sizeof(scb));
                p->context->Unmap(p->smaaCB, 0);
            }
            p->context->VSSetConstantBuffers(0, 1, &p->smaaCB);
            p->context->PSSetConstantBuffers(0, 1, &p->smaaCB);
            ID3D11SamplerState* samps[2] = { p->sampLinear, p->sampPoint };
            p->context->PSSetSamplers(0, 2, samps);
            const float clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ID3D11ShaderResourceView* noSrv[5] = {};

            // 1. Edges (the edge pass discards pixels without one).
            p->context->ClearRenderTargetView(p->smaaEdgesRtv, clear0);
            p->context->OMSetRenderTargets(1, &p->smaaEdgesRtv, nullptr);
            p->context->VSSetShader(p->smaaEdgeVS, nullptr, 0);
            p->context->PSSetShader(p->smaaEdgePS, nullptr, 0);
            p->context->PSSetShaderResources(0, 1, &p->sceneSrv);
            p->context->Draw(3, 0);

            // 2. Blending weights.
            p->context->ClearRenderTargetView(p->smaaBlendRtv, clear0);
            p->context->OMSetRenderTargets(1, &p->smaaBlendRtv, nullptr);
            p->context->VSSetShader(p->smaaWeightVS, nullptr, 0);
            p->context->PSSetShader(p->smaaWeightPS, nullptr, 0);
            ID3D11ShaderResourceView* weightIn[3] = { p->smaaEdgesSrv, p->smaaAreaSrv, p->smaaSearchSrv };
            p->context->PSSetShaderResources(1, 3, weightIn);
            p->context->Draw(3, 0);

            // 3. Neighbourhood blending, into the back buffer or, with the
            // CRT shader still to come, into smaaOut.
            ID3D11RenderTargetView* out = crt ? p->smaaOutRtv : p->rtv;
            p->context->PSSetShaderResources(0, 5, noSrv);
            p->context->OMSetRenderTargets(1, &out, nullptr);
            p->context->VSSetShader(p->smaaBlendVS, nullptr, 0);
            p->context->PSSetShader(p->smaaBlendPS, nullptr, 0);
            ID3D11ShaderResourceView* blendIn[5] = { p->sceneSrv, nullptr, nullptr, nullptr, p->smaaBlendSrv };
            p->context->PSSetShaderResources(0, 5, blendIn);
            if (s_postTrace > 0) crashlog_mark("video: post draw (smaa)");
            p->context->Draw(3, 0);
            p->context->PSSetShaderResources(0, 5, noSrv);
            srv = p->smaaOutSrv;
        }

        if (!p->smaaActive || crt) {
        p->context->OMSetRenderTargets(1, &p->rtv, nullptr);
        p->context->VSSetShader(p->postVS, nullptr, 0);
        p->context->PSSetShader(crt ? p->crtPS : p->copyPS, nullptr, 0);
        p->context->PSSetShaderResources(0, 1, &srv);
        p->context->PSSetSamplers(0, 1, &p->sampLinear);
        if (crt) {
            CrtConstantBuffer cb;
            cb.rect[0] = (float)p->cx;    cb.rect[1] = (float)p->cy;
            cb.rect[2] = (float)p->width; cb.rect[3] = (float)p->height;
            cb.src[0] = 320.0f; cb.src[1] = 240.0f; cb.src[2] = 0.0f; cb.src[3] = 0.0f;
            if (p->wideFrame) {
                // Mod: 16:9 pan-and-scan frame - the CRT covers the 16:9 box,
                // and its scanlines follow the visible part of the 240 lines.
                LONG ox, ew; float sh;
                p->WideBox(&ox, &ew, &sh);
                cb.rect[0] = (float)ox; cb.rect[1] = 0.0f;
                cb.rect[2] = (float)ew; cb.rect[3] = (float)p->bbH;
                cb.src[1] = 240.0f * (float)p->bbH / sh;
            }
            cb.size[0] = (float)p->bbW;   cb.size[1] = (float)p->bbH;
            cb.size[2] = 1.0f / (float)p->bbW; cb.size[3] = 1.0f / (float)p->bbH;
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(p->context->Map(p->crtCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &cb, sizeof(cb));
                p->context->Unmap(p->crtCB, 0);
            }
            p->context->PSSetConstantBuffers(0, 1, &p->crtCB);
        }
        if (s_postTrace > 0) crashlog_mark(crt ? "video: post draw (crt)" : "video: post draw (copy)");
        p->context->Draw(3, 0);

        ID3D11ShaderResourceView* none = nullptr;
        p->context->PSSetShaderResources(0, 1, &none);
        }
    }

    HRESULT phr = p->swapChain->Present(g_bVSync ? 1 : 0, 0);
    if (p->sceneDirty) {
        // A Video Options change: rebuild here, on the main thread's own
        // stack (see SetSmaa). CreateScene binds the new target itself.
        p->sceneDirty = FALSE;
        p->CreateScene();
    }
    if (s_postTrace > 0) {
        crashlog_mark("video: post present done");
        s_postTrace--;
    }
    static int s_presentErrLogged = 0;
    if (FAILED(phr) && !s_presentErrLogged) {
        s_presentErrLogged = 1;
        char m[128];
        sprintf(m, "video: Present failed hr=0x%08lX removed-reason=0x%08lX",
                (unsigned long)phr,
                (unsigned long)(p->device ? p->device->GetDeviceRemovedReason() : 0));
        crashlog_mark(m);
    }
    p->wideFrame = FALSE;
    p->BindCurrent();
}

// Port-added (Video Options). Record the setting; the scene target is rebuilt
// at the end of the next Present().
//
// Never rebuild it here. The F2 menu applies its settings from inside
// game_loop, which runs as a TaskScheduler task on one of the engine's own
// 256 KB stacks, not the thread's real stack. Creating multisampled surfaces
// runs deep into the graphics driver, which uses structured exceptions
// internally; Windows refuses to dispatch an exception whose stack pointer
// lies outside the thread's registered stack and kills the process on the
// spot - no handler runs, so nothing reached crash.log. Present() is called
// from main_loop on the real stack, so the driver work is safe there.
void MarniDX::SetSmaa(BOOL on)
{
    Impl* p = m_pImpl;
    if (!p) return;
    p->smaa = on ? TRUE : FALSE;
    if (p->ready) p->sceneDirty = TRUE;
}

BOOL MarniDX::GetSmaa() const { return m_pImpl ? m_pImpl->smaaActive : FALSE; }

void MarniDX::SetCrtShader(BOOL on)
{
    Impl* p = m_pImpl;
    if (!p) return;
    p->crtShader = on ? TRUE : FALSE;
    if (p->ready) p->sceneDirty = TRUE;     // see SetSmaa
}

BOOL MarniDX::GetCrtShader() const { return m_pImpl ? m_pImpl->crtShader : FALSE; }

// ============================================================================
// Texture handles
// ============================================================================
MarniHandle MarniDX::CreateTexture(int width, int height, int bpp,
                                   const void* pixelData,
                                   int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (!p || !p->device || width <= 0 || height <= 0 || !pixelData)
        return MARNI_NULL_HANDLE;

    DWORD* rgba = ConvertToRGBA8(width, height, bpp, pixelData);
    if (!rgba) return MARNI_NULL_HANDLE;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width           = (UINT)width;
    td.Height          = (UINT)height;
    td.MipLevels       = 1;
    td.ArraySize       = 1;
    td.Format          = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage           = D3D11_USAGE_DEFAULT;
    td.BindFlags       = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem     = rgba;
    sd.SysMemPitch = (UINT)width * 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = p->device->CreateTexture2D(&td, &sd, &tex);
    free(rgba);
    if (FAILED(hr) || !tex) return MARNI_NULL_HANDLE;

    D3D11_SHADER_RESOURCE_VIEW_DESC sdv = {};
    sdv.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    sdv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sdv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    hr = p->device->CreateShaderResourceView(tex, &sdv, &srv);
    if (FAILED(hr) || !srv) { tex->Release(); return MARNI_NULL_HANDLE; }

    int idx = p->AllocSlot(tex, srv, width, height);
    if (idx == 0) return MARNI_NULL_HANDLE;
    if (outWidth)  *outWidth  = width;
    if (outHeight) *outHeight = height;
    return (MarniHandle)idx;
}

MarniHandle MarniDX::CreateTextureFromBits(CMarniBits* bits)
{
    if (!bits) return MARNI_NULL_HANDLE;
    // The bits surface is RGBA host memory laid out in m_pitch rows. We
    // re-interleave via the existing ConvertToRGBA8 path by treating the
    // surface as 32-bit RGBA.
    int w = (int)bits->m_width;
    int h = (int)bits->m_height;
    if (w <= 0 || h <= 0 || bits->m_bitDepth != 32) return MARNI_NULL_HANDLE;
    return CreateTexture(w, h, 32, bits->m_pPixelData, nullptr, nullptr);
}

BOOL MarniDX::UpdateTexturePixels(MarniHandle tex, const void* pixelData,
                                  int width, int height, int bpp)
{
    Impl* p = m_pImpl;
    if (!p || !p->context || !pixelData) return FALSE;
    if (tex <= 0 || tex >= MARNI_MAX_TEXTURES) return FALSE;
    TexSlot& t = p->slots[tex];
    if (!t.tex) return FALSE;
    if (width != t.width || height != t.height) return FALSE;

    // Convert the source to RGBA8 and upload via an staging update subrect.
    DWORD* rgba = ConvertToRGBA8(width, height, bpp, pixelData);
    if (!rgba) return FALSE;

    // UpdateSubresource expects contiguous rows of the source format. Since
    // our texture is already R8G8B8A8, write the converted buffer directly.
    D3D11_BOX dstBox = { 0, 0, 0, (UINT)width, (UINT)height, 1 };
    p->context->UpdateSubresource(t.tex, 0, &dstBox, rgba, (UINT)width * 4, 0);
    free(rgba);
    return TRUE;
}

void MarniDX::GetTextureSize(MarniHandle tex, int* w, int* h) const
{
    Impl* p = m_pImpl;
    if (!p || tex <= 0 || tex >= MARNI_MAX_TEXTURES) { if (w)*w=0; if(h)*h=0; return; }
    const TexSlot& t = p->slots[tex];
    if (w) *w = t.width;
    if (h) *h = t.height;
}

void MarniDX::DestroyTexture(MarniHandle tex)
{
    Impl* p = m_pImpl;
    if (!p) return;
    if ((int)tex <= 0 || (int)tex >= MARNI_MAX_TEXTURES) return;
    p->ReleaseSlot((int)tex);
    // Clear font cache if it pointed here.
    if (p->fontHandle == tex) {
        p->fontHandle = MARNI_NULL_HANDLE; p->fontW = 0; p->fontH = 0;
    }
}

MarniHandle MarniDX::WhiteTexture() const
{
    // DrawQuadInternal already maps MARNI_NULL_HANDLE to the internal white
    // SRV, so callers that need an explicit "white" handle can pass zero and
    // get the same result. This accessor returns the sentinel directly.
    return MARNI_NULL_HANDLE;
}

// ============================================================================
// Drawing
// ============================================================================
static void DrawQuadInternal(MarniDX::Impl* p, const QuadVertex verts[6],
                             MarniHandle tex, MarniSampler sampler, MarniBlend blend)
{
    if (!p || !p->context || !p->quadVB || !p->spriteCB || !p->quadVS
        || !p->quadPS || !p->quadLayout) return;

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(p->context->Map(p->quadVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, verts, sizeof(QuadVertex) * 6);
    p->context->Unmap(p->quadVB, 0);

    SpriteConstantBuffer cb;
    // Y-down ortho (screen-space): origin top-left, +Y goes down.
    // bottom=height, top=0 so that pos.y=0 -> NDC +1 (top) and
    // pos.y=height -> NDC -1 (bottom). Without this swap the image
    // is rendered vertically flipped.
    BuildOrthoMatrix(&cb.mvp[0][0], 0.0f, (float)p->width,
                                   (float)p->height, 0.0f);
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (SUCCEEDED(p->context->Map(p->spriteCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        memcpy(cm.pData, &cb, sizeof(cb));
        p->context->Unmap(p->spriteCB, 0);
    }

    UINT stride = sizeof(QuadVertex), offset = 0;
    p->context->IASetVertexBuffers(0, 1, &p->quadVB, &stride, &offset);
    p->context->IASetInputLayout(p->quadLayout);
    p->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->context->VSSetShader(p->quadVS, nullptr, 0);
    p->context->VSSetConstantBuffers(0, 1, &p->spriteCB);
    p->context->PSSetShader(p->quadPS, nullptr, 0);

    ID3D11ShaderResourceView* srv = nullptr;
    if ((int)tex > 0 && (int)tex < MARNI_MAX_TEXTURES)
        srv = p->slots[tex].srv;
    if (!srv) srv = p->whiteSRV;
    if (!srv) return; // no fallback at all -- skip
    p->context->PSSetShaderResources(0, 1, &srv);

    ID3D11SamplerState* s = (sampler == MARNI_SAMPLER_POINT) ? p->sampPoint : p->sampLinear;
    if (!s) s = p->sampLinear;
    p->context->PSSetSamplers(0, 1, &s);

    ID3D11BlendState* bs = p->blendAlpha;
    if (blend == MARNI_BLEND_ADD)       bs = p->blendAdd;
    else if (blend == MARNI_BLEND_DISABLE) bs = p->blendDisabled;
    if (!bs) bs = p->blendAlpha;
    float bf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    p->context->OMSetBlendState(bs, bf, 0xFFFFFFFFu);

    if (p->depthDisabled)
        p->context->OMSetDepthStencilState(p->depthDisabled, 0);

    p->context->Draw(6, 0);
}

void MarniDX::DrawSprite(float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1,
                         DWORD color, MarniHandle tex,
                         MarniSampler sampler, MarniBlend blend)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready) return;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    float x1 = x + w, y1 = y + h;
    QuadVertex verts[6] = {
        { x,  y,  u0, v0, r, g, b, a },
        { x1, y,  u1, v0, r, g, b, a },
        { x,  y1, u0, v1, r, g, b, a },
        { x,  y1, u0, v1, r, g, b, a },
        { x1, y,  u1, v0, r, g, b, a },
        { x1, y1, u1, v1, r, g, b, a },
    };
    DrawQuadInternal(p, verts, tex, sampler, blend);
}

void MarniDX::DrawTriangles(const float* verts, int triCount, MarniHandle tex,
                            MarniSampler sampler, MarniBlend blend)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready || !verts || triCount <= 0) return;
    // quadVB capacity: 6 verts * 512 sprites = 3072 verts = 1024 triangles
    if (triCount > 1024) triCount = 1024;
    // DrawTriangles shares the quad pipeline: same vertex layout, same ortho
    // MVP, but an arbitrary triangle count instead of the fixed 2-tri quad.
    if (!p->context || !p->quadVB || !p->spriteCB || !p->quadVS
        || !p->quadPS || !p->quadLayout) return;

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(p->context->Map(p->quadVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, verts, sizeof(QuadVertex) * 3 * (size_t)triCount);
    p->context->Unmap(p->quadVB, 0);

    SpriteConstantBuffer cb;
    BuildOrthoMatrix(&cb.mvp[0][0], 0.0f, (float)p->width,
                                   (float)p->height, 0.0f);
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (SUCCEEDED(p->context->Map(p->spriteCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        memcpy(cm.pData, &cb, sizeof(cb));
        p->context->Unmap(p->spriteCB, 0);
    }

    UINT stride = sizeof(QuadVertex), offset = 0;
    p->context->IASetVertexBuffers(0, 1, &p->quadVB, &stride, &offset);
    p->context->IASetInputLayout(p->quadLayout);
    p->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->context->VSSetShader(p->quadVS, nullptr, 0);
    p->context->VSSetConstantBuffers(0, 1, &p->spriteCB);
    p->context->PSSetShader(p->quadPS, nullptr, 0);

    ID3D11ShaderResourceView* srv = nullptr;
    if ((int)tex > 0 && (int)tex < MARNI_MAX_TEXTURES)
        srv = p->slots[tex].srv;
    if (!srv) srv = p->whiteSRV;
    if (!srv) return;
    p->context->PSSetShaderResources(0, 1, &srv);

    ID3D11SamplerState* s = (sampler == MARNI_SAMPLER_POINT) ? p->sampPoint : p->sampLinear;
    if (!s) s = p->sampLinear;
    p->context->PSSetSamplers(0, 1, &s);

    ID3D11BlendState* bs = p->blendAlpha;
    if (blend == MARNI_BLEND_ADD)       bs = p->blendAdd;
    else if (blend == MARNI_BLEND_DISABLE) bs = p->blendDisabled;
    if (!bs) bs = p->blendAlpha;
    float bf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    p->context->OMSetBlendState(bs, bf, 0xFFFFFFFFu);

    if (p->depthDisabled)
        p->context->OMSetDepthStencilState(p->depthDisabled, 0);

    p->context->Draw((UINT)(triCount * 3), 0);
}

void MarniDX::DrawTriangles3D(const float* verts, int triCount, MarniHandle tex,
                              MarniSampler sampler, MarniBlend blend,
                              bool depthWrite)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready || !verts || triCount <= 0) return;
    if (triCount > 1024) triCount = 1024;
    if (!p->context || !p->model3DVB || !p->spriteCB || !p->model3DVS
        || !p->quadPS || !p->model3DLayout) return;

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(p->context->Map(p->model3DVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, verts, sizeof(Model3DVertex) * 3 * (size_t)triCount);
    p->context->Unmap(p->model3DVB, 0);

    SpriteConstantBuffer cb;
    BuildOrthoMatrix(&cb.mvp[0][0], 0.0f, (float)p->width,
                                   (float)p->height, 0.0f);
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (SUCCEEDED(p->context->Map(p->spriteCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        memcpy(cm.pData, &cb, sizeof(cb));
        p->context->Unmap(p->spriteCB, 0);
    }

    UINT stride = sizeof(Model3DVertex), offset = 0;
    p->context->IASetVertexBuffers(0, 1, &p->model3DVB, &stride, &offset);
    p->context->IASetInputLayout(p->model3DLayout);
    p->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->context->VSSetShader(p->model3DVS, nullptr, 0);
    p->context->VSSetConstantBuffers(0, 1, &p->spriteCB);
    p->context->PSSetShader(p->quadPS, nullptr, 0);

    ID3D11ShaderResourceView* srv = nullptr;
    if ((int)tex > 0 && (int)tex < MARNI_MAX_TEXTURES)
        srv = p->slots[tex].srv;
    if (!srv) srv = p->whiteSRV;
    if (!srv) return;
    p->context->PSSetShaderResources(0, 1, &srv);

    ID3D11SamplerState* s = (sampler == MARNI_SAMPLER_POINT) ? p->sampPoint : p->sampLinear;
    if (!s) s = p->sampLinear;
    p->context->PSSetSamplers(0, 1, &s);

    ID3D11BlendState* bs = p->blendAlpha;
    if (blend == MARNI_BLEND_ADD)       bs = p->blendAdd;
    else if (blend == MARNI_BLEND_DISABLE) bs = p->blendDisabled;
    if (!bs) bs = p->blendAlpha;
    float bf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    p->context->OMSetBlendState(bs, bf, 0xFFFFFFFFu);

    // depthWrite=false: translucent geometry (water, glass - record +0x68
    // alpha < 1). Test against what is already in the buffer but never write:
    // the original's alpha-blended D3D7 primitives ran with ZWRITEOFF too.
    // Writing would hide the fade polys (ground shadows / blood pools) that
    // sit BEHIND such surfaces - e.g. every shadow under room40E0's water -
    // and would poison the buffer for the far-to-near painter walk around it.
    ID3D11DepthStencilState* ds = depthWrite ? p->depthEnabled : p->depthTestNoWrite;
    if (ds)
        p->context->OMSetDepthStencilState(ds, 0);

    p->context->Draw((UINT)(triCount * 3), 0);

    // Leave the pipeline in the 2D state the rest of the renderer expects.
    if (p->depthDisabled)
        p->context->OMSetDepthStencilState(p->depthDisabled, 0);
}

void MarniDX::DrawTrianglesPersp(const float* verts, int triCount, MarniHandle tex,
                                 MarniSampler sampler, MarniBlend blend,
                                 bool depthTest)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready || !verts || triCount <= 0) return;
    if (triCount > 1024) triCount = 1024;
    if (!p->context || !p->model3DVB || !p->spriteCB || !p->model3DVS
        || !p->quadPS || !p->model3DLayout) return;

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(p->context->Map(p->model3DVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, verts, sizeof(Model3DVertex) * 3 * (size_t)triCount);
    p->context->Unmap(p->model3DVB, 0);

    SpriteConstantBuffer cb;
    BuildOrthoMatrix(&cb.mvp[0][0], 0.0f, (float)p->width,
                                   (float)p->height, 0.0f);
    D3D11_MAPPED_SUBRESOURCE cm = {};
    if (SUCCEEDED(p->context->Map(p->spriteCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        memcpy(cm.pData, &cb, sizeof(cb));
        p->context->Unmap(p->spriteCB, 0);
    }

    UINT stride = sizeof(Model3DVertex), offset = 0;
    p->context->IASetVertexBuffers(0, 1, &p->model3DVB, &stride, &offset);
    p->context->IASetInputLayout(p->model3DLayout);
    p->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->context->VSSetShader(p->model3DVS, nullptr, 0);
    p->context->VSSetConstantBuffers(0, 1, &p->spriteCB);
    p->context->PSSetShader(p->quadPS, nullptr, 0);

    ID3D11ShaderResourceView* srv = nullptr;
    if ((int)tex > 0 && (int)tex < MARNI_MAX_TEXTURES)
        srv = p->slots[tex].srv;
    if (!srv) srv = p->whiteSRV;
    if (!srv) return;
    p->context->PSSetShaderResources(0, 1, &srv);

    ID3D11SamplerState* s = (sampler == MARNI_SAMPLER_POINT) ? p->sampPoint : p->sampLinear;
    if (!s) s = p->sampLinear;
    p->context->PSSetSamplers(0, 1, &s);

    ID3D11BlendState* bs = p->blendAlpha;
    if (blend == MARNI_BLEND_ADD)       bs = p->blendAdd;
    else if (blend == MARNI_BLEND_DISABLE) bs = p->blendDisabled;
    if (!bs) bs = p->blendAlpha;
    float bf[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    p->context->OMSetBlendState(bs, bf, 0xFFFFFFFFu);

    // No depth by default: the shadow draws over the room like a sprite. This
    // used to inherit whatever the caller had set, which held only while the
    // whole 2D pass ran after the 3D one. FlushTmdObjects now interleaves
    // scene sprites between DrawTriangles3D batches, so state it outright
    // rather than depend on that call having restored it.
    //
    // depthTest=true instead clips against the model geometry like the
    // original's Z-buffered viewport quads (test only - never write). The
    // caller supplies real per-corner NDC z in that case.
    if (depthTest) {
        if (p->depthTestNoWrite)
            p->context->OMSetDepthStencilState(p->depthTestNoWrite, 0);
        p->context->Draw((UINT)(triCount * 3), 0);
        if (p->depthDisabled)
            p->context->OMSetDepthStencilState(p->depthDisabled, 0);
        return;
    }
    if (p->depthDisabled)
        p->context->OMSetDepthStencilState(p->depthDisabled, 0);

    p->context->Draw((UINT)(triCount * 3), 0);
}

void MarniDX::DrawRect(int x, int y, int w, int h, DWORD color)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready) return;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;
    float x0 = (float)x, x1 = (float)(x + w);
    float y0 = (float)y, y1 = (float)(y + h);
    QuadVertex verts[6] = {
        { x0, y0, 0.0f, 0.0f, r, g, b, a },
        { x1, y0, 1.0f, 0.0f, r, g, b, a },
        { x0, y1, 0.0f, 1.0f, r, g, b, a },
        { x0, y1, 0.0f, 1.0f, r, g, b, a },
        { x1, y0, 1.0f, 0.0f, r, g, b, a },
        { x1, y1, 1.0f, 1.0f, r, g, b, a },
    };
    // rects use the white texture; encode via NULL handle -> white fallback.
    DrawQuadInternal(p, verts, MARNI_NULL_HANDLE,
                     MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA);
}

void MarniDX::DrawLine(float x0, float y0, float x1, float y1,
                       float thickness, DWORD color)
{
    Impl* p = m_pImpl;
    if (!p || !p->ready) return;

    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8)  & 0xFF) / 255.0f;
    float b = ( color        & 0xFF) / 255.0f;
    float a = ((color >> 24) & 0xFF) / 255.0f;

    // Thicken the segment with a quad perpendicular to its direction.
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    if (len < 0.001f) {
        nx = 1.0f;
        ny = 0.0f;
    } else {
        nx = -dy / len;
        ny =  dx / len;
    }
    float hw = thickness * 0.5f;
    float ox = nx * hw;
    float oy = ny * hw;

    QuadVertex verts[6] = {
        { x0 - ox, y0 - oy, 0.0f, 0.0f, r, g, b, a },
        { x1 - ox, y1 - oy, 1.0f, 0.0f, r, g, b, a },
        { x0 + ox, y0 + oy, 0.0f, 1.0f, r, g, b, a },
        { x0 + ox, y0 + oy, 0.0f, 1.0f, r, g, b, a },
        { x1 - ox, y1 - oy, 1.0f, 0.0f, r, g, b, a },
        { x1 + ox, y1 + oy, 1.0f, 1.0f, r, g, b, a },
    };
    DrawQuadInternal(p, verts, MARNI_NULL_HANDLE,
                     MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA);
}

// ============================================================================
// Readback
// ============================================================================
BOOL MarniDX::CaptureBackbufferToRGBA(void** outPixels, DWORD* outW, DWORD* outH)
{
    Impl* p = m_pImpl;
    if (!outPixels || !outW || !outH) return FALSE;
    *outPixels = nullptr; *outW = 0; *outH = 0;
    if (!p || !p->ready || !p->swapChain || !p->device || !p->context)
        return FALSE;

    // Port-added: with SMAA / the CRT shader the frame being drawn is in the
    // offscreen scene, not the back buffer.
    ID3D11Texture2D* bb = p->ResolvedScene();
    if (bb != nullptr) {
        bb->AddRef();
    } else if (FAILED(p->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
        return FALSE;
    }

    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    DWORD w = bd.Width, h = bd.Height;

    D3D11_TEXTURE2D_DESC sd = bd;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(p->device->CreateTexture2D(&sd, nullptr, &st))) {
        bb->Release(); return FALSE;
    }
    p->context->CopyResource(st, bb);
    bb->Release();

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(p->context->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        st->Release(); return FALSE;
    }
    DWORD bufSize = w * h * 4;
    BYTE* rgba = (BYTE*)malloc(bufSize);
    if (!rgba) { p->context->Unmap(st, 0); st->Release(); return FALSE; }
    BYTE* dst = rgba; BYTE* src = (BYTE*)m.pData;
    for (DWORD y = 0; y < h; y++) {
        memcpy(dst, src, w * 4);
        dst += w * 4;
        src += m.RowPitch;
    }
    p->context->Unmap(st, 0);
    st->Release();
    *outPixels = rgba; *outW = w; *outH = h;
    return TRUE;
}

BOOL MarniDX::ReadTextureRGBA(MarniHandle tex, void** outPixels,
                              int* outWidth, int* outHeight)
{
    Impl* p = m_pImpl;
    if (!outPixels || !outWidth || !outHeight || !p || !p->ready)
        return FALSE;
    *outPixels = nullptr; *outWidth = 0; *outHeight = 0;
    if ((int)tex <= 0 || (int)tex >= MARNI_MAX_TEXTURES) return FALSE;
    ID3D11Texture2D* ttex = p->slots[tex].tex;
    if (!ttex) return FALSE;

    D3D11_TEXTURE2D_DESC d;
    ttex->GetDesc(&d);

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(p->device->CreateTexture2D(&sd, nullptr, &st))) return FALSE;
    p->context->CopyResource(st, ttex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(p->context->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        st->Release(); return FALSE;
    }
    int w = (int)d.Width, h = (int)d.Height;
    BYTE* rgba = (BYTE*)malloc((size_t)w * h * 4);
    if (!rgba) { p->context->Unmap(st, 0); st->Release(); return FALSE; }
    BYTE* dst = rgba; BYTE* src = (BYTE*)m.pData;
    for (int y = 0; y < h; y++) {
        memcpy(dst, src, (size_t)w * 4);
        dst += w * 4;
        src += m.RowPitch;
    }
    p->context->Unmap(st, 0);
    st->Release();
    *outPixels = rgba; *outWidth = w; *outHeight = h;
    return TRUE;
}

// ============================================================================
// Font texture accessor pair
// ============================================================================
void MarniDX::SetFontTexture(MarniHandle tex, int w, int h)
{
    Impl* p = m_pImpl;
    if (!p) return;
    p->fontHandle = tex;
    p->fontW = w;
    p->fontH = h;
}

MarniHandle MarniDX::FontTexture()    const { return m_pImpl ? m_pImpl->fontHandle : MARNI_NULL_HANDLE; }
int  MarniDX::FontTextureWidth()  const { return m_pImpl ? m_pImpl->fontW : 0; }
int  MarniDX::FontTextureHeight() const { return m_pImpl ? m_pImpl->fontH : 0; }
