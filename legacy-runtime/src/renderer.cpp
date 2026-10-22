// Direct3D 11 renderer for the Xenos command stream. See renderer.h / XENOS_REFERENCE.md.
#include "renderer.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "memory.h"
#include "texture_dump.h"
#include "xenos_shader.h"

using Microsoft::WRL::ComPtr;
static void DumpRt(ID3D11Texture2D* t, const char* path);
static void ReplayFrame();

namespace
{
    // ---- registers --------------------------------------------------------------------------
    enum : uint32_t
    {
        kRbSurfaceInfo = 0x2000, kRbColorInfo = 0x2001, kRbDepthInfo = 0x2002, kRbColor1Info = 0x2003,
        kRbColor2Info = 0x2004, kRbColor3Info = 0x2005, kRbDepthControl = 0x2200, kRbBlendControl0 = 0x2201,
        kRbColorControl = 0x2202, kRbModeControl = 0x2208, kRbColorMask = 0x2104, kRbBlendRed = 0x2105,
        kRbStencilRefMask = 0x210D, kRbStencilRefMaskBf = 0x210C, kRbAlphaRef = 0x210E,
        kRbCopyControl = 0x2318, kRbCopyDestBase = 0x2319, kRbCopyDestPitch = 0x231A, kRbCopyDestInfo = 0x231B,
        kRbDepthClear = 0x231D, kRbColorClear = 0x231E,
        kPaScModeCntl = 0x2205, kPaClClipCntl = 0x2204, kPaClVteCntl = 0x2206, kPaVportXScale = 0x210F,
        kPaScWindowOffset = 0x2080, kPaScWindowScissorTl = 0x2081, kPaScWindowScissorBr = 0x2082,
        kPaScScreenScissorTl = 0x200E, kPaScScreenScissorBr = 0x200F, kVgtIndxOffset = 0x2102,
        kShaderConstFetch = 0x4800, kShaderConstBool = 0x4900, kShaderConstLoop = 0x4908,
    };

    const uint32_t* g_regSnap = nullptr;      // replay: register file snapshot overriding the live one
    int g_dbgOverride = -1;                  // replay: PS debug selector (sys.flags[3] = value) or -1
    bool g_noDepthOverride = false;
    bool g_replaying = false;
    constexpr uint32_t kRegSnapWords = 0x4A00;
    uint32_t Reg(uint32_t i) { return (g_regSnap && i < kRegSnapWords) ? g_regSnap[i] : GpuRegRead(i); }
    float RegF(uint32_t i) { uint32_t v = Reg(i); float f; std::memcpy(&f, &v, 4); return f; }
    uint32_t PhysToVirt(uint32_t p) { return guest::kPhysicalBegin + (p & 0x1FFFFFFF); }

    uint64_t Fnv64(const uint32_t* w, size_t n, uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < n; i++)
        {
            h ^= w[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // ---- state ----------------------------------------------------------------------------------
    struct SysCB
    {
        float vpScale[4], vpOffset[4], rtSize[4];
        uint32_t flags[4];
        float alphaRef[4];
        uint32_t boolC[8];
        uint32_t loopC[32];
        uint32_t texSwz[16];
        uint32_t texFlags[16];
        uint32_t vfEndian[96];
    };

    struct ShaderEntry
    {
        XenosShader info;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        bool bad = false;
        uint64_t key = 0;
    };

    struct Surface
    {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11DepthStencilView> dsv;
        uint32_t w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    };

    struct Resolved
    {
        Surface s;
        uint32_t serial = 0;
        uint32_t tag[4] = {};
        bool swapRB = false;
    };

    struct CachedTexture
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        uint64_t contentHash = 0;
        uint32_t frame = 0;
    };

    struct Program
    {
        uint32_t addr = 0, size = 0;
        std::vector<uint32_t> immediate;
    };

    struct RecDraw
    {
        uint32_t initiator = 0, dmaBase = 0, dmaSizeEndian = 0;
        std::vector<uint32_t> immediate;
        std::vector<uint32_t> regs;
        Program prog[2];
        uint64_t psKey = 0;
    };

    struct Renderer
    {
        std::vector<RecDraw> rec;
        bool replayDone = false;
        uint64_t lastKeys[2] = {};
        size_t replayStart = 0;
        std::recursive_mutex lock;
        bool initialised = false, failed = false;
        HWND hwnd = nullptr;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<IDXGISwapChain1> swap;
        ComPtr<ID3D11RenderTargetView> backRtv;
        ComPtr<ID3D11Buffer> sysCb, vsCb, psCb, vtxListBuf;
        ComPtr<ID3D11ShaderResourceView> vtxListSrv;
        uint32_t vtxListCap = 0;
        ComPtr<ID3D11Buffer> vbuf[kVfetchSlots];
        ComPtr<ID3D11ShaderResourceView> vsrv[kVfetchSlots];
        uint32_t vbufCap[kVfetchSlots] = {};
        ComPtr<ID3D11VertexShader> blitVs;
        ComPtr<ID3D11PixelShader> blitPs;
        ComPtr<ID3D11SamplerState> linearClamp;
        std::unordered_map<uint64_t, ShaderEntry> shaders;
        std::map<uint64_t, Surface> colorRts;
        std::map<uint32_t, Surface> depthRts;
        std::map<uint32_t, Resolved> resolved;      // keyed by destination address
        std::unordered_map<uint64_t, CachedTexture> textures;
        std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> samplers;
        std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blends;
        std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> depthStates;
        std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> rasters;
        Program prog[2];
        uint32_t resolveSerial = 1;
        uint32_t resolveCount = 0;
        std::string frameInfo;
        uint32_t dbgFrame = 0;
        uint32_t loadsSince[2] = {};
        uint32_t lastLoads[2] = {};
        uint32_t frame = 0;
        uint32_t draws = 0, frameDraws = 0;
        ComPtr<ID3D11VertexShader> lastVs;
    } g;

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_CLOSE)
        {
            ExitProcess(0);
        }
        return DefWindowProcA(h, m, w, l);
    }

    void WindowThread(std::atomic<HWND>* out)
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "Q4Recomp";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r = { 0, 0, 1280, 720 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND h = CreateWindowA("Q4Recomp", "Quake 4 (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                               r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        out->store(h);
        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    ComPtr<ID3DBlob> Compile(const std::string& src, const char* target, const char* name, std::string* errOut)
    {
        ComPtr<ID3DBlob> code, err;
        const HRESULT hr = D3DCompile(src.data(), src.size(), name, nullptr, nullptr, "main", target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
        if (FAILED(hr))
        {
            if (errOut && err)
                *errOut = std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
            return nullptr;
        }
        return code;
    }

    ComPtr<ID3DBlob> CompileCached(const std::string& src, const char* target, std::string* errOut)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : src) { h ^= c; h *= 1099511628211ull; }
        for (const char* t = target; *t; t++) { h ^= static_cast<unsigned char>(*t); h *= 1099511628211ull; }
        char path[260];
        std::snprintf(path, sizeof(path), "C:\\Quake4\\shadercache\\%016llX.bin", static_cast<unsigned long long>(h));
        if (FILE* f = std::fopen(path, "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            ComPtr<ID3DBlob> blob;
            if (n > 0 && SUCCEEDED(D3DCreateBlob(static_cast<SIZE_T>(n), &blob)) &&
                std::fread(blob->GetBufferPointer(), 1, static_cast<size_t>(n), f) == static_cast<size_t>(n))
            {
                std::fclose(f);
                return blob;
            }
            std::fclose(f);
        }
        auto blob = Compile(src, target, "xenos", errOut);
        if (blob)
        {
            CreateDirectoryA("C:\\Quake4\\shadercache", nullptr);
            if (FILE* f = std::fopen(path, "wb"))
            {
                std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f);
                std::fclose(f);
            }
        }
        return blob;
    }

    const char* kBlitVs = R"(
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O main(uint id : SV_VertexID) { O o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
)";
    const char* kBlitPs = R"(
Texture2D<float4> src : register(t0); SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(src.Sample(smp, uv).rgb, 1); }
)";

    bool Init()
    {
        if (g.initialised)
            return !g.failed;
        g.initialised = true;

        std::atomic<HWND> hwnd{ nullptr };
        std::thread(WindowThread, &hwnd).detach();
        for (int i = 0; i < 500 && !hwnd.load(); i++)
            Sleep(10);
        g.hwnd = hwnd.load();
        if (!g.hwnd)
        {
            g.failed = true;
            return false;
        }

        UINT flags = 0;
        if (std::getenv("Q4_D3DDEBUG"))
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1, D3D11_SDK_VERSION,
                                       &g.dev, nullptr, &g.ctx);
        if (FAILED(hr))
        {
            std::printf("[gfx] D3D11CreateDevice failed: 0x%08X\n", static_cast<unsigned>(hr));
            g.failed = true;
            return false;
        }
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(g.dev.As(&mt)))
            mt->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIDevice> dxgiDev;
        g.dev.As(&dxgiDev);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 1280;
        sd.Height = 720;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(g.dev.Get(), g.hwnd, &sd, nullptr, nullptr, &g.swap);
        if (FAILED(hr))
        {
            std::printf("[gfx] swap chain failed: 0x%08X\n", static_cast<unsigned>(hr));
            g.failed = true;
            return false;
        }
        ComPtr<ID3D11Texture2D> back;
        g.swap->GetBuffer(0, IID_PPV_ARGS(&back));
        g.dev->CreateRenderTargetView(back.Get(), nullptr, &g.backRtv);

        auto makeCb = [&](uint32_t size, ComPtr<ID3D11Buffer>& b)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (size + 15) & ~15u;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g.dev->CreateBuffer(&bd, nullptr, &b);
        };
        makeCb(sizeof(SysCB), g.sysCb);
        makeCb(256 * 16, g.vsCb);
        makeCb(256 * 16, g.psCb);

        std::string err;
        auto vsBlob = Compile(kBlitVs, "vs_5_0", "blit_vs", &err);
        auto psBlob = Compile(kBlitPs, "ps_5_0", "blit_ps", &err);
        if (!vsBlob || !psBlob)
        {
            std::printf("[gfx] blit shader compile failed: %s\n", err.c_str());
            g.failed = true;
            return false;
        }
        g.dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g.blitVs);
        g.dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g.blitPs);

        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        g.dev->CreateSamplerState(&smp, &g.linearClamp);

        std::printf("[gfx] Direct3D 11 renderer initialised\n");
        std::fflush(stdout);
        return true;
    }

    // ---- shaders -------------------------------------------------------------------------------------
    ShaderEntry* GetShader(uint32_t type)
    {
        Program& p = g.prog[type];
        std::vector<uint32_t> words;
        if (!p.immediate.empty())
        {
            words = p.immediate;
        }
        else
        {
            if (!p.size || p.size > 0x1000)
                return nullptr;
            const uint32_t virt = PhysToVirt(p.addr);
            words.resize(p.size);
            for (uint32_t i = 0; i < p.size; i++)
                words[i] = GuestLoad32(virt + i * 4);
        }
        const uint64_t key = Fnv64(words.data(), words.size()) ^ (type ? 0x8000000000000000ull : 0);
        auto it = g.shaders.find(key);
        if (it != g.shaders.end())
            return it->second.bad ? nullptr : &it->second;

        ShaderEntry& e = g.shaders[key];
        e.key = key;
        if (!TranslateXenosShader(type == 0, words.data(), static_cast<uint32_t>(words.size()), e.info))
        {
            std::printf("[gfx] %s shader %016llX: translation failed: %s\n", type ? "pixel" : "vertex",
                        static_cast<unsigned long long>(key), e.info.error.c_str());
            e.bad = true;
            return nullptr;
        }
        if (!e.info.error.empty())
            std::printf("[gfx] %s shader %016llX: warnings: %s\n", type ? "pixel" : "vertex",
                        static_cast<unsigned long long>(key), e.info.error.c_str());

        std::string err;
        auto blob = CompileCached(e.info.hlsl, type ? "ps_5_0" : "vs_5_0", &err);
        if (!blob)
        {
            char path[260];
            std::snprintf(path, sizeof(path), "C:\\Quake4\\dump\\shader_%016llX_%s.hlsl", static_cast<unsigned long long>(key),
                          type ? "ps" : "vs");
            if (FILE* f = std::fopen(path, "w"))
            {
                std::fputs(e.info.hlsl.c_str(), f);
                std::fclose(f);
            }
            std::printf("[gfx] %s shader %016llX: HLSL compile failed (see %s): %.600s\n", type ? "pixel" : "vertex",
                        static_cast<unsigned long long>(key), path, err.c_str());
            e.bad = true;
            return nullptr;
        }
        if (type == 0)
            g.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e.vs);
        else
            g.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e.ps);
        if (std::getenv("Q4_SHADERLOG"))
        {
            char path[260];
            std::snprintf(path, sizeof(path), "C:\\Quake4\\dump\\shader_%016llX_%s.hlsl", static_cast<unsigned long long>(key),
                          type ? "ps" : "vs");
            if (FILE* f = std::fopen(path, "w"))
            {
                std::fputs(e.info.hlsl.c_str(), f);
                std::fclose(f);
            }
        }
        return &e;
    }

    // ---- render targets -------------------------------------------------------------------------------
    DXGI_FORMAT ColorFormat(uint32_t f)
    {
        switch (f)
        {
        case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case 3: case 12: case 5: case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 4: return DXGI_FORMAT_R16G16_UNORM;
        case 6: return DXGI_FORMAT_R16G16_FLOAT;
        case 14: return DXGI_FORMAT_R32_FLOAT;
        case 15: return DXGI_FORMAT_R32G32_FLOAT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    Surface* GetColorRt(uint32_t base, uint32_t fmt, uint32_t w, uint32_t h)
    {
        const uint64_t key = (static_cast<uint64_t>(base) << 8) | fmt;
        Surface& s = g.colorRts[key];
        if (s.tex && s.w == w && s.h == h)
            return &s;
        s = Surface{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = ColorFormat(fmt);
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &s.tex)))
            return nullptr;
        g.dev->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv);
        g.dev->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv);
        s.w = w;
        s.h = h;
        s.fmt = td.Format;
        const float zero[4] = {};
        g.ctx->ClearRenderTargetView(s.rtv.Get(), zero);
        return &s;
    }

    Surface* GetDepthRt(uint32_t base, uint32_t w, uint32_t h)
    {
        Surface& s = g.depthRts[base];
        if (s.tex && s.w == w && s.h == h)
            return &s;
        s = Surface{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &s.tex)))
            return nullptr;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g.dev->CreateDepthStencilView(s.tex.Get(), &dd, &s.dsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv);
        s.w = w;
        s.h = h;
        g.ctx->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        return &s;
    }

    void SurfaceSize(uint32_t& w, uint32_t& h)
    {
        w = Reg(kRbSurfaceInfo) & 0x3FFF;
        const uint32_t br = Reg(kPaScWindowScissorBr);
        const uint32_t sbr = Reg(kPaScScreenScissorBr);
        h = std::max<uint32_t>((br >> 16) & 0x3FFF, sbr >> 16 & 0x7FFF);
        const uint32_t bw = std::max<uint32_t>(br & 0x3FFF, sbr & 0x7FFF);
        if (w == 0 || w > 4096) w = bw;
        if (w == 0 || w > 4096) w = 1280;
        if (h == 0 || h > 4096) h = 720;
    }

    // ---- textures -------------------------------------------------------------------------------------
    uint32_t TextureSpanBytes(const uint32_t f[6])
    {
        const uint32_t w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1;
        const uint32_t fmt = f[1] & 0x3F;
        uint32_t bpp = 4;
        if (fmt == 2 || fmt == 8 || fmt == 9) bpp = 1;
        else if (fmt == 3 || fmt == 4 || fmt == 5 || fmt == 10 || fmt == 15 || fmt == 24) bpp = 2;
        else if (fmt == 18 || fmt == 26 || fmt == 37) bpp = fmt == 18 ? 1 : 8;
        else if (fmt == 19 || fmt == 20 || fmt == 35 || fmt == 38) bpp = fmt >= 35 ? 16 : 1;
        const uint32_t pw = (w + 31) & ~31u, ph = (h + 31) & ~31u;
        return pw * ph * bpp;
    }

    ID3D11ShaderResourceView* GetTexture(const uint32_t f[6], uint32_t* swzFlags)
    {
        if ((f[0] & 3) != 2)
            return nullptr;
        const uint32_t base = ((f[1] >> 12) << 12);
        // A previously resolved render target?
        auto rit = g.resolved.find(base & 0x1FFFFFFF);
        if (rit != g.resolved.end())
        {
            Resolved& r = rit->second;
            const uint32_t virt = PhysToVirt(base);
            if (GuestLoad32(virt) == r.tag[0] && GuestLoad32(virt + 4) == r.tag[1] && GuestLoad32(virt + 8) == r.tag[2])
            {
                if (swzFlags) *swzFlags = r.swapRB ? 1u : 0u;
                return r.s.srv.Get();
            }
        }

        const uint32_t w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1;
        if (!base || w > 4096 || h > 4096)
            return nullptr;
        const uint64_t key = (static_cast<uint64_t>(base) << 32) ^ (f[0] & 0x80000000u ? 1 : 0) ^ (f[1] & 0x3F) << 1 ^
                             (static_cast<uint64_t>(f[2]) << 8) ^ ((f[0] >> 22) & 0x1FF) << 20;
        // content hash over the span (strided for large textures)
        const uint32_t span = std::min<uint32_t>(TextureSpanBytes(f), 64u << 20);
        const uint32_t virt = PhysToVirt(base);
        uint64_t ch = 1469598103934665603ull;
        const uint8_t* mem = g_guestBase + virt;
        const uint32_t stride = span > 65536 ? (span / 16384) & ~3u : 4;
        for (uint32_t i = 0; i + 4 <= span; i += stride)
        {
            uint32_t v;
            std::memcpy(&v, mem + i, 4);
            ch = (ch ^ v) * 1099511628211ull;
        }
        ch ^= span;

        CachedTexture& ct = g.textures[key];
        if (ct.srv && ct.contentHash == ch)
            return ct.srv.Get();

        std::vector<uint8_t> rgba;
        uint32_t tw = 0, th = 0;
        if (!TextureDecode(f, rgba, tw, th, "bind"))
        {
            rgba.assign(4 * 4 * 4, 128);
            tw = th = 4;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = tw;
        td.Height = th;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> tex;
        {
            const HRESULT thr = g.dev->CreateTexture2D(&td, nullptr, &tex);
            if (FAILED(thr))
            {
                std::printf("[gfx] CreateTexture2D failed 0x%08X for %ux%u\n", static_cast<unsigned>(thr), tw, th);
                return nullptr;
            }
        }
        g.ctx->UpdateSubresource(tex.Get(), 0, nullptr, rgba.data(), tw * 4, 0);
        ComPtr<ID3D11ShaderResourceView> srv;
        g.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        g.ctx->GenerateMips(srv.Get());
        if (std::getenv("Q4_TEXVERIFY"))
        {
            static int nv = 0;
            if (nv < 8)
            {
                char fn[96];
                std::snprintf(fn, sizeof(fn), "C:\\Quake4\\dump\\gputex_%d.png", nv++);
                DumpRt(tex.Get(), fn);
            }
        }
        ct.srv = srv;
        ct.contentHash = ch;
        (void)swzFlags;
        return ct.srv.Get();
    }

    ID3D11SamplerState* GetSampler(const uint32_t f[6])
    {
        const uint32_t clampX = (f[0] >> 10) & 7, clampY = (f[0] >> 13) & 7;
        const uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3, mip = (f[3] >> 23) & 3;
        const uint64_t key = clampX | (clampY << 3) | (mag << 6) | (min << 8) | (mip << 10);
        auto it = g.samplers.find(key);
        if (it != g.samplers.end())
            return it->second.Get();
        auto addr = [](uint32_t c)
        {
            switch (c)
            {
            case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
            case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
            case 6: case 7: return D3D11_TEXTURE_ADDRESS_BORDER;
            case 3: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
            default: return D3D11_TEXTURE_ADDRESS_CLAMP;
            }
        };
        D3D11_SAMPLER_DESC sd{};
        const bool lmin = min != 0, lmag = mag != 0, lmip = mip == 1 || mip == 3;
        sd.Filter = static_cast<D3D11_FILTER>((lmin ? 0x10 : 0) | (lmag ? 0x4 : 0) | (lmip ? 0x1 : 0));
        sd.AddressU = addr(clampX);
        sd.AddressV = addr(clampY);
        sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = mip == 2 ? 0.0f : D3D11_FLOAT32_MAX;
        sd.MaxAnisotropy = 1;
        ComPtr<ID3D11SamplerState> s;
        g.dev->CreateSamplerState(&sd, &s);
        g.samplers[key] = s;
        return s.Get();
    }

    // ---- fixed-function state ----------------------------------------------------------------------
    D3D11_BLEND BlendFactor(uint32_t f, bool alpha)
    {
        switch (f)
        {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 6: return D3D11_BLEND_SRC_ALPHA;
        case 7: return D3D11_BLEND_INV_SRC_ALPHA;
        case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 10: return D3D11_BLEND_DEST_ALPHA;
        case 11: return D3D11_BLEND_INV_DEST_ALPHA;
        case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
        case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
        case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
        default: return D3D11_BLEND_ONE;
        }
    }
    D3D11_BLEND_OP BlendOp(uint32_t o)
    {
        switch (o)
        {
        case 1: return D3D11_BLEND_OP_SUBTRACT;
        case 2: return D3D11_BLEND_OP_MIN;
        case 3: return D3D11_BLEND_OP_MAX;
        case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
        default: return D3D11_BLEND_OP_ADD;
        }
    }
    D3D11_STENCIL_OP StencilOp(uint32_t o)
    {
        static const D3D11_STENCIL_OP m[] = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE,
                                              D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
                                              D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR };
        return m[o & 7];
    }
    D3D11_COMPARISON_FUNC Cmp(uint32_t f) { return static_cast<D3D11_COMPARISON_FUNC>((f & 7) + 1); }

    void ApplyFixedState(bool depthOnlyOrNoColor, uint32_t numTargets)
    {
        // blend
        const uint32_t mask = Reg(kRbColorMask);
        uint64_t bkey = mask;
        D3D11_BLEND_DESC bd{};
        bd.IndependentBlendEnable = TRUE;
        for (uint32_t i = 0; i < 4; i++)
        {
            static const uint32_t regs[4] = { 0x2201, 0x2209, 0x220A, 0x220B };
            const uint32_t bc = Reg(regs[i]);
            bkey = bkey * 1000003u + bc;
            const uint32_t cs = bc & 31, co = (bc >> 5) & 7, cd = (bc >> 8) & 31;
            const uint32_t as = (bc >> 16) & 31, ao = (bc >> 21) & 7, ad = (bc >> 24) & 31;
            auto& rt = bd.RenderTarget[i];
            rt.BlendEnable = !(cs == 1 && cd == 0 && co == 0 && as == 1 && ad == 0 && ao == 0);
            rt.SrcBlend = BlendFactor(cs, false);
            rt.DestBlend = BlendFactor(cd, false);
            rt.BlendOp = BlendOp(co);
            rt.SrcBlendAlpha = BlendFactor(as, true);
            rt.DestBlendAlpha = BlendFactor(ad, true);
            rt.BlendOpAlpha = BlendOp(ao);
            rt.RenderTargetWriteMask = static_cast<UINT8>((mask >> (4 * i)) & 0xF);
        }
        auto& bs = g.blends[bkey];
        if (!bs)
            g.dev->CreateBlendState(&bd, &bs);
        const float bf[4] = { RegF(kRbBlendRed), RegF(kRbBlendRed + 1), RegF(kRbBlendRed + 2), RegF(kRbBlendRed + 3) };
        g.ctx->OMSetBlendState(bs.Get(), bf, 0xFFFFFFFF);

        // depth / stencil
        const uint32_t dc = Reg(kRbDepthControl);
        const uint32_t sr = Reg(kRbStencilRefMask);
        uint64_t dkey = dc | (static_cast<uint64_t>(sr & 0xFFFF00) << 32) | (g_noDepthOverride ? (1ull << 63) : 0);
        auto& ds = g.depthStates[dkey];
        if (!ds)
        {
            D3D11_DEPTH_STENCIL_DESC dd{};
            const bool noDepth = g_noDepthOverride || std::getenv("Q4_NODEPTH") != nullptr;
            dd.DepthEnable = noDepth ? FALSE : ((dc >> 1) & 1);
            dd.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = ((dc >> 4) & 7) == 2 ? D3D11_COMPARISON_LESS_EQUAL : Cmp((dc >> 4) & 7); // EQUAL needs bit-identical depth between passes
            dd.StencilEnable = dc & 1;
            dd.StencilReadMask = static_cast<UINT8>((sr >> 8) & 0xFF);
            dd.StencilWriteMask = static_cast<UINT8>((sr >> 16) & 0xFF);
            dd.FrontFace = { StencilOp((dc >> 11) & 7), StencilOp((dc >> 17) & 7), StencilOp((dc >> 14) & 7), Cmp((dc >> 8) & 7) };
            if ((dc >> 7) & 1)
                dd.BackFace = { StencilOp((dc >> 23) & 7), StencilOp((dc >> 29) & 7), StencilOp((dc >> 26) & 7), Cmp((dc >> 20) & 7) };
            else
                dd.BackFace = dd.FrontFace;
            g.dev->CreateDepthStencilState(&dd, &ds);
        }
        g.ctx->OMSetDepthStencilState(ds.Get(), sr & 0xFF);

        // rasterizer
        const uint32_t mc = Reg(kPaScModeCntl);
        const uint32_t cc = Reg(kPaClClipCntl);
        const bool eqDepth = ((dc >> 1) & 1) && (((dc >> 4) & 7) == 2);
        uint64_t rkey = (mc & 0x1FFF) | ((cc >> 16) & 1) << 13 | (eqDepth ? (1ull << 14) : 0);
        rkey = rkey * 1000003ull + Reg(0x2380); rkey = rkey * 1000003ull + Reg(0x2381); rkey = rkey * 1000003ull + Reg(0x2382); rkey = rkey * 1000003ull + Reg(0x2383);
        auto& rs = g.rasters[rkey];
        if (!rs)
        {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            const bool cullF = mc & 1, cullB = (mc >> 1) & 1;
            rd.CullMode = cullF && cullB ? D3D11_CULL_BACK : cullF ? D3D11_CULL_FRONT : cullB ? D3D11_CULL_BACK : D3D11_CULL_NONE;
            if (std::getenv("Q4_NOCULL")) rd.CullMode = D3D11_CULL_NONE;
            if (std::getenv("Q4_FLIPCULL") && rd.CullMode != D3D11_CULL_NONE) rd.CullMode = rd.CullMode == D3D11_CULL_BACK ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
            rd.FrontCounterClockwise = ((mc >> 2) & 1) ? TRUE : FALSE;
            rd.DepthClipEnable = !((cc >> 16) & 1);
            rd.ScissorEnable = TRUE;
            {
                // Polygon offset (front unless front faces are culled), as Xenia: bias in D24 units, slope in 1/16 subpixel units.
                float scale = 0.0f, offset = 0.0f;
                const bool pf = (mc >> 11) & 1, pb = (mc >> 12) & 1;
                if (pf && !cullF) { scale = RegF(0x2380); offset = RegF(0x2381); }
                if (pb && !cullB && scale == 0.0f && offset == 0.0f) { scale = RegF(0x2382); offset = RegF(0x2383); }
                if (scale != 0.0f || offset != 0.0f)
                {
                    int32_t bias = static_cast<int32_t>(std::ceil(std::fabs(offset) * 16777215.0f));
                    rd.DepthBias = offset < 0 ? -bias : bias;
                    rd.SlopeScaledDepthBias = scale * (1.0f / 16.0f);
                }
            }
            if (eqDepth)
            {
                static const int bias = std::getenv("Q4_EQBIAS") ? std::atoi(std::getenv("Q4_EQBIAS")) : -256;
                rd.DepthBias = bias; // pull 'equal' passes slightly closer so they survive LEQUAL against the pre-pass
                rd.SlopeScaledDepthBias = -1.0f;
            }
            g.dev->CreateRasterizerState(&rd, &rs);
        }
        g.ctx->RSSetState(rs.Get());
        (void)depthOnlyOrNoColor;
        (void)numTargets;
    }

    void UpdateCb(ID3D11Buffer* b, const void* data, size_t size)
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g.ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, data, size);
            g.ctx->Unmap(b, 0);
        }
    }

    void UploadVertexBuffer(uint32_t slot, const uint8_t* data, uint32_t bytes)
    {
        bytes = (bytes + 15) & ~15u;
        if (bytes < 64) bytes = 64;
        if (g.vbufCap[slot] < bytes)
        {
            uint32_t cap = 4096;
            while (cap < bytes) cap <<= 1;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = cap;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            g.vbuf[slot].Reset();
            g.vsrv[slot].Reset();
            if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.vbuf[slot])))
                return;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R32_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sv.BufferEx.NumElements = cap / 4;
            sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            g.dev->CreateShaderResourceView(g.vbuf[slot].Get(), &sv, &g.vsrv[slot]);
            g.vbufCap[slot] = cap;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g.ctx->Map(g.vbuf[slot].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, data, bytes);
            g.ctx->Unmap(g.vbuf[slot].Get(), 0);
        }
    }

    // ---- resolve --------------------------------------------------------------------------------------
    void DoResolve()
    {
        g.resolveCount++;
        const uint32_t ctl = Reg(kRbCopyControl);
        const uint32_t srcSel = ctl & 7;
        const uint32_t cmd = (ctl >> 20) & 3;
        const uint32_t destBase = Reg(kRbCopyDestBase) & 0x1FFFFFFF;
        const uint32_t destInfo = Reg(kRbCopyDestInfo);
        const uint32_t pitchReg = Reg(kRbCopyDestPitch);
        {
            static int logged = 0;
            if (logged++ < 40)
                std::printf("[gfx] resolve: src=%u cmd=%u dest=%08X info=%08X pitch=%08X scissor=%08X/%08X frame=%u\n", srcSel, cmd,
                            destBase, destInfo, pitchReg, Reg(kPaScWindowScissorTl), Reg(kPaScWindowScissorBr), g.frame);
        }
        uint32_t sw, sh;
        SurfaceSize(sw, sh);
        const uint32_t tl = Reg(kPaScWindowScissorTl), br = Reg(kPaScWindowScissorBr);
        uint32_t x0 = tl & 0x3FFF, y0 = (tl >> 16) & 0x3FFF, x1 = br & 0x3FFF, y1 = (br >> 16) & 0x3FFF;
        if (x1 <= x0 || y1 <= y0) { x0 = 0; y0 = 0; x1 = sw; y1 = sh; }
        x1 = std::min(x1, sw);
        y1 = std::min(y1, sh);
        const uint32_t w = x1 - x0, h = y1 - y0;

        if (cmd != 3)
        {
            Surface* src = nullptr;
            static const uint32_t infoRegs[4] = { kRbColorInfo, kRbColor1Info, kRbColor2Info, kRbColor3Info };
            const uint32_t colorInfo = Reg(infoRegs[srcSel & 3]);
            if (srcSel == 4)
            {
                const uint32_t di = Reg(kRbDepthInfo);
                src = GetDepthRt((di & 0x7FF) | (((di >> 11) & 1) << 11), sw, sh);
            }
            else
            {
                src = GetColorRt((colorInfo & 0x7FF) | (((colorInfo >> 11) & 1) << 11), (colorInfo >> 16) & 15, sw, sh);
            }
            if (src && src->srv)
            {
                Resolved& r = g.resolved[destBase];
                const DXGI_FORMAT dfmt = srcSel == 4 ? DXGI_FORMAT_R32_FLOAT : src->fmt;
                if (!r.s.tex || r.s.w != w || r.s.h != h || r.s.fmt != dfmt)
                {
                    r.s = Surface{};
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = w;
                    td.Height = h;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = dfmt;
                    td.SampleDesc.Count = 1;
                    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    g.dev->CreateTexture2D(&td, nullptr, &r.s.tex);
                    g.dev->CreateRenderTargetView(r.s.tex.Get(), nullptr, &r.s.rtv);
                    g.dev->CreateShaderResourceView(r.s.tex.Get(), nullptr, &r.s.srv);
                    r.s.w = w;
                    r.s.h = h;
                    r.s.fmt = dfmt;
                }
                if (srcSel != 4)
                {
                    D3D11_BOX box = { x0, y0, 0, x1, y1, 1 };
                    g.ctx->CopySubresourceRegion(r.s.tex.Get(), 0, 0, 0, 0, src->tex.Get(), 0, &box);
                }
                else
                {
                    // depth -> R32F via the blit shader (reads .r)
                    ID3D11RenderTargetView* rtv = r.s.rtv.Get();
                    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
                    D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1 };
                    g.ctx->RSSetViewports(1, &vp);
                    g.ctx->VSSetShader(g.blitVs.Get(), nullptr, 0);
                    g.ctx->PSSetShader(g.blitPs.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* sv = src->srv.Get();
                    g.ctx->PSSetShaderResources(0, 1, &sv);
                    ID3D11SamplerState* sp = g.linearClamp.Get();
                    g.ctx->PSSetSamplers(0, 1, &sp);
                    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    g.ctx->IASetInputLayout(nullptr);
                    g.ctx->Draw(3, 0);
                    ID3D11ShaderResourceView* nul = nullptr;
                    g.ctx->PSSetShaderResources(0, 1, &nul);
                }
                // tag the destination memory so a later CPU overwrite invalidates this entry
                const uint32_t virt = PhysToVirt(destBase);
                r.swapRB = (destInfo >> 24) & 1;
                r.serial = ++g.resolveSerial;
                r.tag[0] = 0x52534C56u;
                r.tag[1] = r.serial;
                r.tag[2] = destBase;
                GuestStore32(virt, r.tag[0]);
                GuestStore32(virt + 4, r.tag[1]);
                GuestStore32(virt + 8, r.tag[2]);
            }
        }
        (void)pitchReg;

        // clears performed by the copy
        if ((ctl >> 8) & 1)
        {
            const uint32_t ci = Reg(kRbColorInfo);
            Surface* s = GetColorRt((ci & 0x7FF) | (((ci >> 11) & 1) << 11), (ci >> 16) & 15, sw, sh);
            if (s)
            {
                const uint32_t c = Reg(kRbColorClear);
                const float col[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f,
                                       ((c >> 24) & 255) / 255.0f };
                g.ctx->ClearRenderTargetView(s->rtv.Get(), col);
            }
        }
        if ((ctl >> 9) & 1)
        {
            const uint32_t di = Reg(kRbDepthInfo);
            Surface* s = GetDepthRt((di & 0x7FF) | (((di >> 11) & 1) << 11), sw, sh);
            if (s)
            {
                const uint32_t c = Reg(kRbDepthClear);
                g.ctx->ClearDepthStencilView(s->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                             static_cast<float>(c >> 8) / 16777215.0f, static_cast<UINT8>(c & 255));
            }
        }
    }
}


// ---- host input -------------------------------------------------------------------------------------------
HostPad RendererPollInput()
{
    HostPad pad;
    static HMODULE xi = LoadLibraryA("xinput1_4.dll");
    typedef DWORD(WINAPI * XInputGetStateFn)(DWORD, void*);
    static XInputGetStateFn xget = xi ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState")) : nullptr;
    if (xget)
    {
        struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st{};
        if (xget(0, &st) == ERROR_SUCCESS)
        {
            pad.buttons = st.buttons;
            pad.leftTrigger = st.lt;
            pad.rightTrigger = st.rt;
            pad.thumb[0] = st.lx; pad.thumb[1] = st.ly; pad.thumb[2] = st.rx; pad.thumb[3] = st.ry;
            pad.active = true;
        }
    }

    HWND hw = g.hwnd;
    if (!hw || GetForegroundWindow() != hw)
        return pad;

    auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    auto setBtn = [&](int vk, uint16_t mask) { if (key(vk)) pad.buttons |= mask; };
    setBtn(VK_SPACE, 0x1000);   // A  jump
    setBtn(VK_LCONTROL, 0x2000); // B  crouch
    setBtn('C', 0x2000);
    setBtn('R', 0x4000);        // X  reload / use
    setBtn('E', 0x4000);
    setBtn(VK_TAB, 0x8000);     // Y  next weapon
    setBtn('Q', 0x0100);        // LB
    setBtn('F', 0x0200);        // RB
    setBtn(VK_RETURN, 0x0010);  // Start
    setBtn(VK_ESCAPE, 0x0020);  // Back
    setBtn(VK_LSHIFT, 0x0040);  // left stick click (sprint)
    setBtn(VK_UP, 0x0001); setBtn(VK_DOWN, 0x0002); setBtn(VK_LEFT, 0x0004); setBtn(VK_RIGHT, 0x0008);
    if (key(VK_LBUTTON)) pad.rightTrigger = 255; // fire
    if (key(VK_RBUTTON)) pad.leftTrigger = 255;  // aim

    int lx = 0, ly = 0;
    if (key('A')) lx -= 32767;
    if (key('D')) lx += 32767;
    if (key('S')) ly -= 32767;
    if (key('W')) ly += 32767;
    if (lx || ly) { pad.thumb[0] = static_cast<int16_t>(lx); pad.thumb[1] = static_cast<int16_t>(ly); }

    // Mouse look: relative movement since the last poll, cursor recentred in the client area.
    static bool capture = true, f1Prev = false;
    const bool f1 = key(VK_F1);
    if (f1 && !f1Prev) capture = !capture;
    f1Prev = f1;
    if (capture)
    {
        RECT rc;
        GetClientRect(hw, &rc);
        POINT c = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        POINT cur;
        GetCursorPos(&cur);
        ClientToScreen(hw, &c);
        const int dx = cur.x - c.x, dy = cur.y - c.y;
        SetCursorPos(c.x, c.y);
        auto clampS = [](int v) { return static_cast<int16_t>(std::max(-32767, std::min(32767, v))); };
        if (dx || dy)
        {
            pad.thumb[2] = clampS(dx * 2500);
            pad.thumb[3] = clampS(-dy * 2500);
        }
    }
    pad.active = true;
    return pad;
}

static void DumpRt(ID3D11Texture2D* t, const char* path)
{
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &st)))
        return;
    g.ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return;
    std::vector<uint8_t> px(static_cast<size_t>(td.Width) * td.Height * 4);
    for (uint32_t y = 0; y < td.Height; y++)
        std::memcpy(&px[static_cast<size_t>(y) * td.Width * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, td.Width * 4);
    g.ctx->Unmap(st.Get(), 0);
    {
        size_t nz = 0;
        unsigned mx[4] = {};
        for (size_t k = 0; k + 3 < px.size(); k += 4)
        {
            if (px[k] | px[k + 1] | px[k + 2]) nz++;
            for (int c = 0; c < 4; c++) mx[c] = std::max<unsigned>(mx[c], px[k + c]);
        }
        std::printf("[dump] %s nonzero=%zu max=(%u,%u,%u,%u)\n", path, nz, mx[0], mx[1], mx[2], mx[3]);
    }
    WritePngRgba(path, px.data(), td.Width, td.Height);
}

// ---- public entry points ------------------------------------------------------------------------
void RendererSetShaderAddress(uint32_t type, uint32_t physAddr, uint32_t sizeDwords)
{
    if (type > 1) return;
    std::lock_guard<std::recursive_mutex> l(g.lock);
    g.prog[type].addr = physAddr;
    g.loadsSince[type]++;
    g.prog[type].size = sizeDwords;
    g.prog[type].immediate.clear();
}

void RendererSetShaderImmediate(uint32_t type, const uint32_t* words, uint32_t sizeDwords)
{
    if (type > 1) return;
    std::lock_guard<std::recursive_mutex> l(g.lock);
    g.prog[type].immediate.assign(words, words + sizeDwords);
    g.loadsSince[type]++;
    g.prog[type].size = sizeDwords;
}

void RendererDraw(uint32_t initiator, const uint32_t* immediate, uint32_t immediateDwords, uint32_t dmaBase,
                  uint32_t dmaSizeEndian)
{
    std::lock_guard<std::recursive_mutex> l(g.lock);
    if (!Init())
        return;

    {
        static const bool recOn = std::getenv("Q4_REPLAY") != nullptr;
        if (recOn && !g_replaying)
        {
            RecDraw r;
            r.initiator = initiator;
            r.dmaBase = dmaBase;
            r.dmaSizeEndian = dmaSizeEndian;
            if (immediate && immediateDwords)
                r.immediate.assign(immediate, immediate + immediateDwords);
            r.regs.resize(kRegSnapWords);
            for (uint32_t i = 0; i < kRegSnapWords; i++)
                r.regs[i] = GpuRegRead(i);
            r.prog[0] = g.prog[0];
            r.prog[1] = g.prog[1];
            g.rec.push_back(std::move(r));
        }
    }

    g.lastLoads[0] = g.loadsSince[0];
    g.lastLoads[1] = g.loadsSince[1];
    g.loadsSince[0] = g.loadsSince[1] = 0;
    const uint32_t mode = Reg(kRbModeControl) & 7;
    if (g.frameInfo.size() < 600)
    {
        char tmp[80];
        std::snprintf(tmp, sizeof(tmp), "[m%u p%u n%u dest%X cc%X] ", mode, initiator & 0x3F, initiator >> 16,
                      Reg(kRbCopyDestBase) & 0x1FFFFFFF, Reg(kRbCopyControl));
        g.frameInfo += tmp;
    }
    if (mode == 6)
    {
        DoResolve();
        if (!g_replaying && (Reg(kRbCopyDestBase) & 0x1FFFFFFF) == 0x9000)
        {
            static const char* spec = std::getenv("Q4_REPLAY");
            if (spec && !g.replayDone)
            {
                unsigned minFrame = 0, minDraws = 100, lastK = 400;
                std::sscanf(spec, "%u,%u,%u", &minFrame, &minDraws, &lastK);
                g.replayStart = g.rec.size() > lastK ? g.rec.size() - lastK : 0;
                size_t lit = 0;
                for (const RecDraw& rd : g.rec) lit += rd.psKey == 0x39E40A89DBB7E384ull;
                if (g.frame >= minFrame && g.rec.size() >= minDraws && lit >= 30)
                {
                    g.replayDone = true;
                    ReplayFrame();
                }
            }
            g.rec.clear();
        }
        return;
    }
    {
        static uint32_t modeCount[8] = {};
        static uint32_t total = 0;
        modeCount[mode]++;
        if (++total % 2000 == 0 && std::getenv("Q4_GFXSTATS"))
            std::printf("[gfx] draw modes: 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u\n", modeCount[0], modeCount[1], modeCount[2], modeCount[3], modeCount[4], modeCount[5], modeCount[6], modeCount[7]);
    }
    if (mode != 4 && mode != 5)
        return;

    const uint32_t prim = initiator & 0x3F;
    const uint32_t src = (initiator >> 6) & 3;
    const bool idx32 = (initiator >> 11) & 1;
    const uint32_t count = initiator >> 16;
    if (count == 0 || count > 0x100000)
        return;

    // ---- index list ------------------------------------------------------------------------------
    std::vector<uint32_t> idx(count);
    const uint32_t indxOffset = Reg(kVgtIndxOffset);
    if (src == 2)
    {
        for (uint32_t i = 0; i < count; i++)
            idx[i] = i;
    }
    else if (src == 0)
    {
        const uint32_t virt = PhysToVirt(dmaBase & ~3u);
        for (uint32_t i = 0; i < count; i++)
            idx[i] = idx32 ? GuestLoad32(virt + i * 4) : GuestLoad16(virt + i * 2);
    }
    else if (immediate)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            if (idx32)
                idx[i] = i < immediateDwords ? immediate[i] : 0;
            else
            {
                const uint32_t w = (i >> 1) < immediateDwords ? immediate[i >> 1] : 0;
                idx[i] = (i & 1) ? (w & 0xFFFF) : (w >> 16);
            }
        }
    }
    else
        return;
    (void)dmaSizeEndian;
    uint32_t maxIdx = 0;
    if (src == 0 && (dmaBase & 0x1FFFFFFF) == 0)
        return;
    {
        // Xenos: index + VGT_INDX_OFFSET, low 24 bits, then clamped to [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX].
        const uint32_t vmin = Reg(0x2101), vmax = Reg(0x2100);
        static const bool noClamp = std::getenv("Q4_NOIDXCLAMP") != nullptr;
        for (uint32_t& v : idx)
        {
            v = (v + indxOffset) & 0xFFFFFF;
            if (!noClamp)
                v = std::min(std::max(v, vmin), vmax);
            maxIdx = std::max(maxIdx, v);
        }
    }

    // ---- primitive expansion -> vertex list ------------------------------------------------------------
    std::vector<uint32_t> list; // uint4 per vertex
    auto push = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t w) { list.push_back(a); list.push_back(b); list.push_back(c); list.push_back(w); };
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (prim)
    {
    case 4:
        for (uint32_t i = 0; i + 2 < count; i += 3) { push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); push(idx[i + 2], 0, 0, 0); }
        break;
    case 6:
        for (uint32_t i = 0; i + 2 < count; i++)
        {
            if (i & 1) { push(idx[i + 1], 0, 0, 0); push(idx[i], 0, 0, 0); push(idx[i + 2], 0, 0, 0); }
            else       { push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); push(idx[i + 2], 0, 0, 0); }
        }
        break;
    case 5: case 0xF:
        for (uint32_t i = 1; i + 1 < count; i++) { push(idx[0], 0, 0, 0); push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); }
        break;
    case 0xD:
        for (uint32_t i = 0; i + 3 < count; i += 4)
        {
            push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); push(idx[i + 2], 0, 0, 0);
            push(idx[i], 0, 0, 0); push(idx[i + 2], 0, 0, 0); push(idx[i + 3], 0, 0, 0);
        }
        break;
    case 0xE:
        for (uint32_t i = 0; i + 3 < count; i += 2)
        {
            push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); push(idx[i + 2], 0, 0, 0);
            push(idx[i + 1], 0, 0, 0); push(idx[i + 3], 0, 0, 0); push(idx[i + 2], 0, 0, 0);
        }
        break;
    case 8: // rectangle list: corners A, B, C and D = B + C - A
        for (uint32_t i = 0; i + 2 < count; i += 3)
        {
            const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
            push(a, b, c, 1); push(a, b, c, 2); push(a, b, c, 3);
            push(a, b, c, 1); push(a, b, c, 3); push(a, b, c, 4);
        }
        break;
    case 2: case 3:
        topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        if (prim == 2) for (uint32_t i = 0; i + 1 < count; i += 2) { push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); }
        else for (uint32_t i = 0; i + 1 < count; i++) { push(idx[i], 0, 0, 0); push(idx[i + 1], 0, 0, 0); }
        break;
    case 1:
        topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        for (uint32_t i = 0; i < count; i++) push(idx[i], 0, 0, 0);
        break;
    default:
    {
        static int warned = 0;
        if (warned++ < 8) std::printf("[gfx] unhandled primitive type %u (%u indices)\n", prim, count);
        return;
    }
    }
    const uint32_t vcount = static_cast<uint32_t>(list.size() / 4);
    if (!vcount || maxIdx > 0x400000)
        return; // garbage indices (stale or unfinished data)

    // ---- shaders ---------------------------------------------------------------------------------------
    ShaderEntry* vs = GetShader(0);
    ShaderEntry* ps = GetShader(1);
    g.lastKeys[0] = vs ? vs->key : 0;
    g.lastKeys[1] = ps ? ps->key : 0;
    if (!g_replaying && !g.rec.empty())
        g.rec.back().psKey = g.lastKeys[1];
    if (!vs || !ps || !vs->vs || !ps->ps)
        return;
    {
        static const bool skipFlat = std::getenv("Q4_SKIPFLAT") != nullptr;
        if (skipFlat && mode == 4 && prim != 8 && ps->info.numRegs <= 1 && !ps->info.texMask2D && Reg(kRbBlendControl0) == 0x01010101)
            return;
    }

    {
        // Xenos draws nothing when both faces are culled (shadow-volume style passes rely on this).
        const bool polygonal = prim == 4 || prim == 5 || prim == 6 || prim == 7 || prim == 8 || prim == 0xD || prim == 0xE || prim == 0xF;
        const uint32_t mcul = Reg(kPaScModeCntl);
        if (polygonal && (mcul & 1) && (mcul & 2))
            return;
    }
    // ---- render targets ------------------------------------------------------------------------------------
    uint32_t sw, sh;
    SurfaceSize(sw, sh);
    ID3D11RenderTargetView* rtvs[4] = {};
    uint32_t nrt = 0;
    const uint32_t colorMask = Reg(kRbColorMask);
    if (mode == 4)
    {
        static const uint32_t infoRegs[4] = { kRbColorInfo, kRbColor1Info, kRbColor2Info, kRbColor3Info };
        for (uint32_t i = 0; i < 4; i++)
        {
            if (i > 0 && !((colorMask >> (4 * i)) & 0xF))
                continue;
            const uint32_t ci = Reg(infoRegs[i]);
            Surface* s = GetColorRt((ci & 0x7FF) | (((ci >> 11) & 1) << 11), (ci >> 16) & 15, sw, sh);
            if (s)
            {
                rtvs[i] = s->rtv.Get();
                nrt = i + 1;
            }
        }
    }
    const uint32_t di = Reg(kRbDepthInfo);
    Surface* depth = GetDepthRt((di & 0x7FF) | (((di >> 11) & 1) << 11), sw, sh);
    g.ctx->OMSetRenderTargets(4, rtvs, depth ? depth->dsv.Get() : nullptr);
    ApplyFixedState(mode == 5, nrt);

    D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(sw), static_cast<float>(sh), 0.0f, 1.0f };
    g.ctx->RSSetViewports(1, &vp);
    const uint32_t stl = Reg(kPaScWindowScissorTl), sbr = Reg(kPaScWindowScissorBr);
    D3D11_RECT sc = { static_cast<LONG>(stl & 0x3FFF), static_cast<LONG>((stl >> 16) & 0x3FFF),
                      static_cast<LONG>(sbr & 0x3FFF), static_cast<LONG>((sbr >> 16) & 0x3FFF) };
    if (sc.right <= sc.left || sc.bottom <= sc.top) sc = { 0, 0, static_cast<LONG>(sw), static_cast<LONG>(sh) };
    g.ctx->RSSetScissorRects(1, &sc);

    // ---- constants --------------------------------------------------------------------------------------
    SysCB sys{};
    const uint32_t vte = Reg(kPaClVteCntl);
    const float scaleDefault[3] = { 1, 1, 1 };
    for (uint32_t k = 0; k < 3; k++)
    {
        const bool sEn = (vte >> (2 * k)) & 1, oEn = (vte >> (2 * k + 1)) & 1;
        sys.vpScale[k] = sEn ? RegF(kPaVportXScale + 2 * k) : scaleDefault[k];
        sys.vpOffset[k] = oEn ? RegF(kPaVportXScale + 2 * k + 1) : 0.0f;
    }
    if ((Reg(kPaScModeCntl) >> 16) & 1)
    {
        const uint32_t wo = Reg(kPaScWindowOffset);
        int32_t wx = static_cast<int32_t>(wo & 0x7FFF), wy = static_cast<int32_t>((wo >> 16) & 0x7FFF);
        if (wx & 0x4000) wx -= 0x8000;
        if (wy & 0x4000) wy -= 0x8000;
        sys.vpOffset[0] += static_cast<float>(wx);
        sys.vpOffset[1] += static_cast<float>(wy);
    }
    sys.rtSize[0] = static_cast<float>(sw);
    sys.rtSize[1] = static_cast<float>(sh);
    sys.rtSize[2] = (vte >> 8) & 1 ? 1.0f : 0.0f;
    sys.rtSize[3] = (vte >> 9) & 1 ? 1.0f : 0.0f;
    sys.flags[0] = (vte >> 10) & 1;
    { static const int dbgReg = std::getenv("Q4_PSDBG") ? std::atoi(std::getenv("Q4_PSDBG")) : -1; sys.flags[3] = g_dbgOverride >= 0 ? static_cast<uint32_t>(g_dbgOverride) : (dbgReg >= 0 ? static_cast<uint32_t>(dbgReg) + 1 : 0); }
    {
        const uint32_t cc = Reg(kRbColorControl);
        sys.flags[1] = ((cc >> 3) & 1) ? ((cc & 7) + 1) : 0; // compare func + 1 (1=never..8=always)
        // shader uses: 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always; map xenos func f -> f for 1..6, 7 -> 7, 0 never -> 8
        const uint32_t f = cc & 7;
        sys.flags[1] = ((cc >> 3) & 1) ? (f == 0 ? 8 : f) : 0;
        const uint32_t ci = Reg(kRbColorInfo);
        sys.flags[2] = ((ci >> 16) & 15) == 1 ? 1 : 0;
        sys.alphaRef[0] = RegF(kRbAlphaRef);
    }
    for (uint32_t i = 0; i < 8; i++) sys.boolC[i] = Reg(kShaderConstBool + i);
    for (uint32_t i = 0; i < 32; i++) sys.loopC[i] = Reg(kShaderConstLoop + i);

    // textures
    uint32_t fetch[16][6] = {};
    for (uint32_t t = 0; t < 16; t++)
    {
        for (uint32_t k = 0; k < 6; k++)
            fetch[t][k] = Reg(kShaderConstFetch + t * 6 + k);
        sys.texSwz[t] = (fetch[t][3] >> 1) & 0xFFF;
    }
    ID3D11ShaderResourceView* texSrv[16] = {};
    for (uint32_t t = 0; t < 16; t++)
    {
        if (!((ps->info.texMask2D | ps->info.texMask3D | ps->info.texMaskCube) & (1u << t)))
            continue;
        uint32_t fl = 0;
        texSrv[t] = GetTexture(fetch[t], &fl);
        if (fl & 1)
        {
            static const uint32_t nat2act[4] = { 2, 1, 0, 3 };
            uint32_t s = sys.texSwz[t], o = 0;
            for (int k = 0; k < 4; k++)
            {
                uint32_t c = (s >> (3 * k)) & 7;
                if (c < 4) c = nat2act[c];
                o |= c << (3 * k);
            }
            sys.texSwz[t] = o;
        }
    }
    // vertex fetch endianness per slot
    uint32_t vfetchConst[kVfetchSlots][2];
    for (uint32_t s = 0; s < kVfetchSlots; s++)
    {
        vfetchConst[s][0] = Reg(kShaderConstFetch + (s / 3) * 6 + (s % 3) * 2);
        vfetchConst[s][1] = Reg(kShaderConstFetch + (s / 3) * 6 + (s % 3) * 2 + 1);
        sys.vfEndian[s] = vfetchConst[s][1] & 3;
    }
    UpdateCb(g.sysCb.Get(), &sys, sizeof(sys));

    float consts[2][256 * 4];
    for (uint32_t i = 0; i < 256 * 4; i++)
    {
        const uint32_t v0 = Reg(0x4000 + i), v1 = Reg(0x4400 + i);
        std::memcpy(&consts[0][i], &v0, 4);
        std::memcpy(&consts[1][i], &v1, 4);
    }
    UpdateCb(g.vsCb.Get(), consts[0], sizeof(consts[0]));
    UpdateCb(g.psCb.Get(), consts[1], sizeof(consts[1]));

    // vertex buffers
    ID3D11ShaderResourceView* vsrvs[kVfetchSlots + 25] = {};
    for (uint32_t s = 0; s < kVfetchSlots; s++)
    {
        if (!(vs->info.vfetchSlots[s >> 6] & (1ull << (s & 63))))
            continue;
        const uint32_t addr = vfetchConst[s][0] & ~3u;
        const uint32_t sizeBytes = ((vfetchConst[s][1] >> 2) & 0xFFFFFF) * 4;
        uint64_t need = (static_cast<uint64_t>(vs->info.slotExtent[s]) + static_cast<uint64_t>(maxIdx) * vs->info.slotStride[s]) * 4;
        if (std::getenv("Q4_CLAMPVB")) need = std::min<uint64_t>(need, sizeBytes ? sizeBytes : need);
        need = std::min<uint64_t>(need, 64u << 20);
        if (!addr || !need)
            continue;
        {
            static int overflow = 0;
            const uint64_t needW = vs->info.slotExtent[s] + static_cast<uint64_t>(maxIdx) * vs->info.slotStride[s];
            if (std::getenv("Q4_GFXSTATS") && sizeBytes && needW * 4 > sizeBytes && maxIdx > 1000 && overflow++ < 30)
                std::printf("[gfx] vertex fetch overflow: vs %016llX slot %u needs %llu words, buffer %u words (maxIdx %u stride %u prim %u n %u idxoff %u)\n",
                            (unsigned long long)vs->key, s, (unsigned long long)needW, sizeBytes / 4, maxIdx, vs->info.slotStride[s], prim, count, indxOffset);
        }
        UploadVertexBuffer(s, g_guestBase + PhysToVirt(addr), static_cast<uint32_t>(need));
        vsrvs[s] = g.vsrv[s].Get();
    }
    // vertex list
    {
        const uint32_t bytes = vcount * 16;
        if (g.vtxListCap < bytes)
        {
            uint32_t cap = 65536;
            while (cap < bytes) cap <<= 1;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = cap;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 16;
            g.vtxListBuf.Reset();
            g.vtxListSrv.Reset();
            g.dev->CreateBuffer(&bd, nullptr, &g.vtxListBuf);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_UNKNOWN;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sv.Buffer.NumElements = cap / 16;
            g.dev->CreateShaderResourceView(g.vtxListBuf.Get(), &sv, &g.vtxListSrv);
            g.vtxListCap = cap;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g.ctx->Map(g.vtxListBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, list.data(), bytes);
            g.ctx->Unmap(g.vtxListBuf.Get(), 0);
        }
    }

    g.ctx->VSSetShader(vs->vs.Get(), nullptr, 0);
    {
        static const bool flat = std::getenv("Q4_FLATPS") != nullptr;
        static ComPtr<ID3D11PixelShader> flatPs;
        if (flat && !flatPs)
        {
            std::string e;
            auto b = Compile("struct PsIn { float4 pos : SV_Position; float4 i[16] : TEXCOORD0; float4 psz : PSIZE_; };float4 main(PsIn p) : SV_Target { return float4(1, 0.5, 0.25, 1); }", "ps_5_0", "flat", &e);
            if (b) g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &flatPs);
        }
        static const bool flatTex = std::getenv("Q4_FLATTEX") != nullptr;
        if (flatTex && !flatPs) { std::string e2; auto b2 = Compile("struct PsIn { float4 pos : SV_Position; float4 i[16] : TEXCOORD0; float4 psz : PSIZE_; };float4 main(PsIn p) : SV_Target { return float4(1, 0.5, 0.25, 1); }", "ps_5_0", "flat", &e2); if (b2) g.dev->CreatePixelShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, &flatPs); }
        const bool useFlat = (flat || (flatTex && ps->info.texMask2D > 7)) && flatPs;
        g.ctx->PSSetShader(useFlat ? flatPs.Get() : ps->ps.Get(), nullptr, 0);
    }
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(topo);
    ID3D11Buffer* cbs[2] = { g.sysCb.Get(), g.vsCb.Get() };
    g.ctx->VSSetConstantBuffers(0, 2, cbs);
    ID3D11Buffer* pcbs[2] = { g.sysCb.Get(), g.psCb.Get() };
    g.ctx->PSSetConstantBuffers(0, 2, pcbs);
    g.ctx->VSSetShaderResources(0, kVfetchSlots, vsrvs);
    ID3D11ShaderResourceView* lst = g.vtxListSrv.Get();
    g.ctx->VSSetShaderResources(kVtxListSlot, 1, &lst);

    ID3D11ShaderResourceView* psrvs[48] = {};
    ID3D11SamplerState* samps[16] = {};
    for (uint32_t t = 0; t < 16; t++)
    {
        const uint32_t bit = 1u << t;
        if (!((ps->info.texMask2D | ps->info.texMask3D | ps->info.texMaskCube) & bit))
            continue;
        ID3D11ShaderResourceView* v = texSrv[t];
        if (ps->info.texMask2D & bit) psrvs[kTex2DBase + t] = v;
        samps[t] = GetSampler(fetch[t]);
    }
    g.ctx->PSSetShaderResources(0, 48, psrvs);
    g.ctx->PSSetSamplers(0, 16, samps);

    g.ctx->Draw(vcount, 0);
    g.draws++;
    g.frameDraws++;
    {
        static std::map<std::pair<uint64_t, uint64_t>, uint32_t> pairCount;
        pairCount[{ vs->key, ps->key }]++;
        if (g.draws % 4000 == 0 && std::getenv("Q4_GFXSTATS"))
        {
            for (auto& kv : pairCount)
                std::printf("[gfx] pair vs=%016llX ps=%016llX x%u (vsregs=%u psregs=%u tex=%04X)\n", (unsigned long long)kv.first.first,
                            (unsigned long long)kv.first.second, kv.second, g.shaders[kv.first.first].info.numRegs,
                            g.shaders[kv.first.second].info.numRegs, g.shaders[kv.first.second].info.texMask2D);
        }
    }

    if (std::getenv("Q4_TEXDRAWS") && ps->info.texMask2D > 7 && rtvs[0] && g.frame >= static_cast<uint32_t>(std::atoi(std::getenv("Q4_TEXDRAWS"))))
    {
        static int nTex = 0;
        if (nTex < 30)
        {
            std::printf("[texdraw] #%d frame %u prim=%u n=%u vs=%016llX ps=%016llX tex=%04X blend=%08X zctl=%08X scissor=%08X/%08X\n",
                        nTex, g.frame, prim, count, (unsigned long long)vs->key, (unsigned long long)ps->key, ps->info.texMask2D,
                        Reg(kRbBlendControl0), Reg(kRbDepthControl), Reg(kPaScWindowScissorTl), Reg(kPaScWindowScissorBr));
            std::printf("    loads since previous draw: vs %u ps %u; vs %08X/%u imm=%zu ps %08X/%u imm=%zu\n", g.lastLoads[0], g.lastLoads[1],
                        g.prog[0].addr, g.prog[0].size, g.prog[0].immediate.size(), g.prog[1].addr, g.prog[1].size, g.prog[1].immediate.size());
            for (uint32_t gI = 28; gI < 32; gI++)
                std::printf("    fetchgrp %u: %08X %08X %08X %08X %08X %08X\n", gI, Reg(kShaderConstFetch + gI * 6), Reg(kShaderConstFetch + gI * 6 + 1),
                            Reg(kShaderConstFetch + gI * 6 + 2), Reg(kShaderConstFetch + gI * 6 + 3), Reg(kShaderConstFetch + gI * 6 + 4),
                            Reg(kShaderConstFetch + gI * 6 + 5));
            for (uint32_t r = 0; r < 16; r++)
                std::printf("    VS c%u: %g %g %g %g\n", 75 + r, consts[0][(75 + r) * 4], consts[0][(75 + r) * 4 + 1],
                            consts[0][(75 + r) * 4 + 2], consts[0][(75 + r) * 4 + 3]);
            for (uint32_t s = 0; s < kVfetchSlots; s++)
            {
                if (!(vs->info.vfetchSlots[s >> 6] & (1ull << (s & 63))))
                    continue;
                const uint32_t va = PhysToVirt(vfetchConst[s][0] & ~3u);
                const uint32_t st = vs->info.slotStride[s];
                std::printf("    slot %u: maxIdx %u stride %u -> needs %llu words, buffer has %u (endian %u)\n", s, maxIdx, st,
                            (unsigned long long)(vs->info.slotExtent[s] + (uint64_t)maxIdx * st), (vfetchConst[s][1] >> 2) & 0xFFFFFF, vfetchConst[s][1] & 3);
                for (uint32_t k = 0; k < std::min<uint32_t>(count, 3); k++)
                {
                    std::printf("    slot %u (addr %08X size %u, stride %u) idx %u:", s, vfetchConst[s][0], (vfetchConst[s][1] >> 2) & 0xFFFFFF, st, idx[k]);
                    for (uint32_t j = 0; j < st && j < 8; j++)
                    {
                        const uint32_t u = GuestLoad32(va + (idx[k] * st + j) * 4);
                        float f;
                        std::memcpy(&f, &u, 4);
                        std::printf(" %g", f);
                    }
                    std::printf("\n");
                }
            }
            std::printf("    colormask=%08X colorinfo=%08X depthinfo=%08X surf=%08X mode=%08X  bools: %08X %08X %08X %08X  ps c0..c3: %g %g %g %g | %g %g %g %g\n", Reg(kRbColorMask), Reg(kRbColorInfo), Reg(kRbDepthInfo), Reg(kRbSurfaceInfo), Reg(kRbModeControl), sys.boolC[0], sys.boolC[1], sys.boolC[2], sys.boolC[4],
                        consts[1][0], consts[1][1], consts[1][2], consts[1][3], consts[1][4], consts[1][5], consts[1][6], consts[1][7]);
            for (uint32_t t = 0; t < 8; t++)
                if (ps->info.texMask2D & (1u << t))
                {
                    std::printf("    tex%u: %08X %08X %08X %08X %08X %08X\n", t, fetch[t][0], fetch[t][1], fetch[t][2], fetch[t][3],
                                fetch[t][4], fetch[t][5]);
                    std::printf("    tex%u srv=%p sampler=%p resolved=%d\n", t, (void*)texSrv[t], (void*)samps[t], (int)g.resolved.count(((fetch[t][1] >> 12) << 12) & 0x1FFFFFFF));
                    char tn[96];
                    std::snprintf(tn, sizeof(tn), "C:\\Quake4\\dump\\texdraw%02d_t%u.png", nTex, t);
                    if (nTex < 6)
                        TextureDump(fetch[t], tn);
                }
            ComPtr<ID3D11Resource> res;
            rtvs[0]->GetResource(&res);
            ComPtr<ID3D11Texture2D> t2;
            res.As(&t2);
            char fn[96];
            std::snprintf(fn, sizeof(fn), "C:\\Quake4\\dump\\texdraw_%02d.png", nTex);
            DumpRt(t2.Get(), fn);
            nTex++;
        }
    }
    static const int dbgFrame = std::getenv("Q4_GFXLOG") ? std::atoi(std::getenv("Q4_GFXLOG")) : -1;
    if (dbgFrame >= 0 && g.dbgFrame != 0 && g.frame >= g.dbgFrame && g.frame < g.dbgFrame + 25)
    {
        static int n = 0;
        if (n < 150)
        {
            std::printf("[gfxdraw] vsA=%08X/%u psA=%08X/%u imm=%zu/%zu\n", g.prog[0].addr, g.prog[0].size, g.prog[1].addr, g.prog[1].size, g.prog[0].immediate.size(), g.prog[1].immediate.size());
            std::printf("[gfxdraw] #%d frame %u prim=%u n=%u verts=%u vsk=%016llX psk=%016llX vs=%u regs ps=%u regs tex=%04X rt0=%p mode=%u zctl=%08X blend=%08X "
                        "vpx=(%g,%g) vpy=(%g,%g) vte=%08X color=%08X\n",
                        n, g.frame, prim, count, vcount, (unsigned long long)vs->key, (unsigned long long)ps->key, vs->info.numRegs, ps->info.numRegs, ps->info.texMask2D, (void*)rtvs[0],
                        mode, Reg(kRbDepthControl), Reg(kRbBlendControl0), sys.vpScale[0], sys.vpOffset[0], sys.vpScale[1],
                        sys.vpOffset[1], vte, Reg(kRbColorInfo), Reg(kRbColorMask));
            if (vs->info.vfetchSlots[1] & (1ull << 31))
            {
                const uint32_t va = PhysToVirt(vfetchConst[95][0] & ~3u);
                const uint32_t st = vs->info.slotStride[95];
                std::printf("    slot95 sweep (stride %u):", st);
                for (uint32_t k = 0; k <= maxIdx; k += std::max<uint32_t>(1, maxIdx / 14))
                {
                    uint32_t u[3];
                    float f[3];
                    for (int j = 0; j < 3; j++) { u[j] = GuestLoad32(va + (k * st + j) * 4); std::memcpy(&f[j], &u[j], 4); }
                    std::printf(" [%u](%g %g %g)", k, f[0], f[1], f[2]);
                }
                std::printf("\n");
            }
            std::printf("    colormask=%08X stencilref=%08X bf=%08X modectl=%08X sqprog=%08X clipcntl=%08X colorctl=%08X\n", Reg(kRbColorMask), Reg(kRbStencilRefMask),
                        Reg(kRbStencilRefMaskBf), Reg(kRbModeControl), Reg(0x2180), Reg(kPaClClipCntl), Reg(kRbColorControl));
            std::printf("    VS c0..c1: %g %g %g %g | %g %g %g %g  PS c0: %g %g %g %g\n", consts[0][0], consts[0][1], consts[0][2], consts[0][3], consts[0][4], consts[0][5], consts[0][6], consts[0][7], consts[1][0], consts[1][1], consts[1][2], consts[1][3]);
            for (uint32_t s = 0; s < kVfetchSlots && count <= 400; s++)
            {
                if (!(vs->info.vfetchSlots[s >> 6] & (1ull << (s & 63))))
                    continue;
                const uint32_t va = PhysToVirt(vfetchConst[s][0] & ~3u);
                const uint32_t stride = vs->info.slotStride[s];
                for (uint32_t k = 0; k < std::min<uint32_t>(count, 2); k++)
                {
                    std::printf("   (slot %u buffer %u words, maxIdx %u, addr %08X)", s, (vfetchConst[s][1] >> 2) & 0xFFFFFF, maxIdx, vfetchConst[s][0]);
                    std::printf("   slot %u stride %u idx %u:", s, stride, idx[k]);
                    for (uint32_t j = 0; j < stride && j < 12; j++)
                    {
                        const uint32_t u = GuestLoad32(va + (idx[k] * stride + j) * 4);
                        float f;
                        std::memcpy(&f, &u, 4);
                        std::printf(" %g", f);
                    }
                    std::printf("\n");
                }
            }
            if (prim == 8)
            {
                const uint32_t va = PhysToVirt(vfetchConst[0][0] & ~3u);
                const uint32_t stride = vs->info.slotStride[0] ? vs->info.slotStride[0] : 7;
                for (uint32_t k = 0; k < 3; k++)
                {
                    float f[7];
                    for (int j = 0; j < 7; j++)
                    {
                        const uint32_t u = GuestLoad32(va + (idx[k] * stride + j) * 4);
                        std::memcpy(&f[j], &u, 4);
                    }
                    std::printf("   rect v%u idx=%u: %g %g %g | %g %g %g %g (vfetch0 addr %08X endian %u)\n", k, idx[k], f[0], f[1], f[2],
                                f[3], f[4], f[5], f[6], vfetchConst[0][0], vfetchConst[0][1] & 3);
                }
            }
            if (rtvs[0])
            {
                ComPtr<ID3D11Resource> res;
                rtvs[0]->GetResource(&res);
                ComPtr<ID3D11Texture2D> t2;
                res.As(&t2);
                char fn[96];
                std::snprintf(fn, sizeof(fn), "C:\\Quake4\\dump\\draw_%02d.png", n);
                DumpRt(t2.Get(), fn);
            }
            n++;
        }
    }

    // unbind SRVs that alias render targets
    ID3D11ShaderResourceView* nullSrv[48] = {};
    g.ctx->PSSetShaderResources(0, 48, nullSrv);
}

static size_t RtNonzero(ID3D11Texture2D* t)
{
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &st)))
        return 0;
    g.ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return 0;
    size_t nz = 0;
    for (uint32_t y = 0; y < td.Height; y++)
    {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
        for (uint32_t x = 0; x < td.Width; x++)
            if (row[x * 4] | row[x * 4 + 1] | row[x * 4 + 2]) nz++;
    }
    g.ctx->Unmap(st.Get(), 0);
    return nz;
}

// Replays the draws recorded during the last frame under several debug variants and writes one image per
// variant (dump/replay_<name>.png). Memory is still as the frame left it, so the result is reproducible.
static void ReplayFrame()
{
    struct Variant { int dbg; bool noDepth; const char* name; };
    static const Variant variants[] = {
        { -1, false, "normal" }, { -1, true, "nodepth" }, { 200, true, "i0" }, { 201, true, "i1" }, { 202, true, "i2" },
        { 203, true, "i3" }, { 204, true, "i4" }, { 205, true, "i5" }, { 206, true, "i6" }, { 207, true, "i7" },
        { 1, true, "r0" }, { 2, true, "r1" }, { 3, true, "r2" }, { 4, true, "r3" }, { 5, true, "r4" }, { 6, true, "r5" },
        { 7, true, "r6" }, { 8, true, "r7" }, { 9, true, "r8" }, { 10, true, "r9" }, { 11, true, "r10" }, { 12, true, "r11" },
        { 304, true, "t4fixed" },
    };
    std::printf("[replay] replaying %zu recorded draws" "\n", g.rec.size());
    const Program saved0 = g.prog[0], saved1 = g.prog[1];
    g_replaying = true;
    for (const Variant& v : variants)
    {
        g_dbgOverride = v.dbg;
        g_noDepthOverride = v.noDepth;
        const float zero[4] = {};
        for (auto& kv : g.colorRts) if (kv.second.rtv) g.ctx->ClearRenderTargetView(kv.second.rtv.Get(), zero);
        for (auto& kv : g.depthRts) if (kv.second.dsv) g.ctx->ClearDepthStencilView(kv.second.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        uint32_t lastDest = 0;
        size_t prevNz = 0;
        for (size_t i = g.replayStart; i < g.rec.size(); i++)
        {
            RecDraw& r = g.rec[i];
            g_regSnap = r.regs.data();
            g.prog[0] = r.prog[0];
            g.prog[1] = r.prog[1];
            const uint32_t mode = Reg(kRbModeControl) & 7;
            if (mode == 6)
                lastDest = Reg(kRbCopyDestBase) & 0x1FFFFFFF;
            RendererDraw(r.initiator, r.immediate.empty() ? nullptr : r.immediate.data(), static_cast<uint32_t>(r.immediate.size()),
                         r.dmaBase, r.dmaSizeEndian);
            if (v.dbg == -1 && !v.noDepth && mode != 6)
            {
                uint32_t sw, sh;
                SurfaceSize(sw, sh);
                const uint32_t ci = Reg(kRbColorInfo);
                Surface* sf = GetColorRt((ci & 0x7FF) | (((ci >> 11) & 1) << 11), (ci >> 16) & 15, sw, sh);
                const size_t nz = sf && sf->tex ? RtNonzero(sf->tex.Get()) : 0;
                std::printf("[replay] draw %zu mode=%u prim=%u n=%u vs=%016llX ps=%016llX nonzero %zu (%+lld)" "\n", i, mode, r.initiator & 0x3F,
                            r.initiator >> 16, static_cast<unsigned long long>(g.lastKeys[0]), static_cast<unsigned long long>(g.lastKeys[1]), nz, static_cast<long long>(nz) - static_cast<long long>(prevNz));
                prevNz = nz;
            }
        }
        g_regSnap = nullptr;
        auto it = g.resolved.find(lastDest);
        if (it != g.resolved.end() && it->second.s.tex)
        {
            char fn[128];
            std::snprintf(fn, sizeof(fn), "C:\\Quake4\\dump\\replay_%s.png", v.name);
            DumpRt(it->second.s.tex.Get(), fn);
        }
    }

    if (std::getenv("Q4_REPLAYPROBE"))
    {
        // Per lit draw: draw it alone (no depth test) under a few debug selectors and report the coverage / max value.
        static const int sel[] = { 9, 10, 11, 204, 203, 305 };
        static const char* names[] = { "R8", "R9", "R10", "i4", "i3", "tex5@fixed" };
        int shown = 0;
        g_noDepthOverride = true;
        for (size_t i = g.replayStart; i < g.rec.size() && shown < 12; i++)
        {
            RecDraw& r = g.rec[i];
            if (r.psKey != 0x39E40A89DBB7E384ull)
                continue;
            shown++;
            std::printf("[probe] draw %zu n=%u:", i, r.initiator >> 16);
            for (int k = 0; k < 6; k++)
            {
                g_dbgOverride = sel[k];
                g_regSnap = r.regs.data();
                g.prog[0] = r.prog[0];
                g.prog[1] = r.prog[1];
                uint32_t sw, sh;
                SurfaceSize(sw, sh);
                const uint32_t ci = Reg(kRbColorInfo);
                Surface* sf = GetColorRt((ci & 0x7FF) | (((ci >> 11) & 1) << 11), (ci >> 16) & 15, sw, sh);
                const float zero[4] = {};
                if (sf && sf->rtv) g.ctx->ClearRenderTargetView(sf->rtv.Get(), zero);
                RendererDraw(r.initiator, r.immediate.empty() ? nullptr : r.immediate.data(), static_cast<uint32_t>(r.immediate.size()), r.dmaBase, r.dmaSizeEndian);
                std::printf(" %s=%zu", names[k], sf && sf->tex ? RtNonzero(sf->tex.Get()) : 0);
            }
            std::printf("\n");
        }
        g_noDepthOverride = false;
        g_dbgOverride = -1;
        g_regSnap = nullptr;
    }
    g_regSnap = nullptr;
    g_dbgOverride = -1;
    g_noDepthOverride = false;
    g_replaying = false;
    g.prog[0] = saved0;
    g.prog[1] = saved1;
}

void RendererSwap(uint32_t fetchPtr)
{
    std::lock_guard<std::recursive_mutex> l(g.lock);
    if (!Init())
        return;
    uint32_t f[6];
    for (uint32_t i = 0; i < 6; i++)
        f[i] = GuestLoad32(fetchPtr + i * 4);
    const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
    ID3D11ShaderResourceView* srv = nullptr;
    auto it = g.resolved.find(base);
    if (it != g.resolved.end())
        srv = it->second.s.srv.Get();

    const float black[4] = { 0, 0, 0, 1 };
    ID3D11RenderTargetView* rtv = g.backRtv.Get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->ClearRenderTargetView(rtv, black);
    if (srv)
    {
        D3D11_VIEWPORT vp = { 0, 0, 1280, 720, 0, 1 };
        g.ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc = { 0, 0, 1280, 720 };
        g.ctx->RSSetScissorRects(1, &sc);
        g.ctx->VSSetShader(g.blitVs.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.blitPs.Get(), nullptr, 0);
        g.ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* sp = g.linearClamp.Get();
        g.ctx->PSSetSamplers(0, 1, &sp);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = 0xF;
        static ComPtr<ID3D11BlendState> opaque;
        if (!opaque) g.dev->CreateBlendState(&bd, &opaque);
        g.ctx->OMSetBlendState(opaque.Get(), nullptr, 0xFFFFFFFF);
        D3D11_DEPTH_STENCIL_DESC dd{};
        static ComPtr<ID3D11DepthStencilState> nodepth;
        if (!nodepth) g.dev->CreateDepthStencilState(&dd, &nodepth);
        g.ctx->OMSetDepthStencilState(nodepth.Get(), 0);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = FALSE;
        static ComPtr<ID3D11RasterizerState> rs;
        if (!rs) g.dev->CreateRasterizerState(&rd, &rs);
        g.ctx->RSSetState(rs.Get());
        g.ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nul = nullptr;
        g.ctx->PSSetShaderResources(0, 1, &nul);
    }

    // optional frame capture
    static const int capture = std::getenv("Q4_CAPTURE") ? std::atoi(std::getenv("Q4_CAPTURE")) : -1;
    if (capture >= 0 && g.frame >= static_cast<uint32_t>(capture) && srv && it != g.resolved.end())
    {
        static uint32_t lastCap = 0, nCap = 0;
        if (nCap < 12 && g.frame >= lastCap + (std::getenv("Q4_CAPSTEP") ? std::atoi(std::getenv("Q4_CAPSTEP")) : 150) && it->second.s.fmt == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            lastCap = g.frame;
            nCap++;
            D3D11_TEXTURE2D_DESC td{};
            it->second.s.tex->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            if (SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &st)))
            {
                g.ctx->CopyResource(st.Get(), it->second.s.tex.Get());
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
                {
                    std::vector<uint8_t> px(static_cast<size_t>(td.Width) * td.Height * 4);
                    for (uint32_t y = 0; y < td.Height; y++)
                        std::memcpy(&px[static_cast<size_t>(y) * td.Width * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, td.Width * 4);
                    g.ctx->Unmap(st.Get(), 0);
                    { char fn[96]; std::snprintf(fn, sizeof(fn), "C:\\Quake4\\dump\\frame_%02u.png", nCap); WritePngRgba(fn, px.data(), td.Width, td.Height); WritePngRgba("C:\\Quake4\\dump\\frame.png", px.data(), td.Width, td.Height); }
                    std::printf("[gfx] captured frame %u to dump\\frame.png (%ux%u)\n", g.frame, td.Width, td.Height);
                }
            }
        }
    }

    g.swap->Present(std::getenv("Q4_NOVSYNC") ? 0 : 1, 0);
    g.frame++;
    static int bigFrames = 0;
    {
        static const int armAt = std::getenv("Q4_GFXLOG") ? std::atoi(std::getenv("Q4_GFXLOG")) : -1;
        if (armAt >= 0 && g.dbgFrame == 0 && g.frame >= static_cast<uint32_t>(armAt) && g.frameDraws >= 15)
            g.dbgFrame = g.frame + 1;
    }
    if (g.frameDraws > 20 && bigFrames++ < 30)
        std::printf("[gfx] big frame %u: %u draws\n", g.frame, g.frameDraws);
    static uint32_t lastResolves = 0;
    {
        static const int swapLog = std::getenv("Q4_SWAPLOG") ? std::atoi(std::getenv("Q4_SWAPLOG")) : -1;
        if (swapLog >= 0 && g.frame >= static_cast<uint32_t>(swapLog) && g.frame < static_cast<uint32_t>(swapLog) + 60)
            std::printf("[swaplog] frame %u: draws %u resolves+%u front %s : %s\n", g.frame, g.frameDraws, g.resolveCount - lastResolves, srv ? "resolved" : "none",
                        g.frameInfo.c_str());
        lastResolves = g.resolveCount;
        g.frameInfo.clear();
    }
    if (g.frame <= 3 || g.frame % 300 == 0)
    {
        std::printf("[gfx] swap #%u t=%llums: %u draws, resolves so far %u, front buffer 0x%08X %s\n", g.frame, static_cast<unsigned long long>(GetTickCount64()), g.frameDraws, g.resolveCount, base,
                    srv ? "resolved" : "NOT RESOLVED");
        std::fflush(stdout);
    }
    g.frameDraws = 0;
}
