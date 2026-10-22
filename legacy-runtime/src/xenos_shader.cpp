// Xenos microcode -> HLSL translator. See XENOS_REFERENCE.md for the encodings.
#include "xenos_shader.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct CfInstr
    {
        uint32_t w0 = 0, w1 = 0;
        uint32_t opcode() const { return (w1 >> 12) & 0xF; }
        uint32_t execAddress() const { return w0 & 0xFFF; }
        uint32_t execCount() const { return (w0 >> 12) & 7; }
        uint32_t sequence() const { return (w0 >> 16) & 0xFFF; }
        uint32_t boolAddress() const { return (w1 >> 2) & 0xFF; }
        bool condition() const { return (w1 >> 10) & 1; }
        uint32_t jumpAddress() const { return w0 & 0x1FFF; }
        uint32_t loopId() const { return (w0 >> 16) & 0x1F; }
        bool predicatedBreak() const { return (w0 >> 21) & 1; }
    };

    enum Cf
    {
        kNop = 0, kExec = 1, kExecEnd = 2, kCondExec = 3, kCondExecEnd = 4, kCondExecPred = 5, kCondExecPredEnd = 6,
        kLoopStart = 7, kLoopEnd = 8, kCondCall = 9, kReturn = 10, kCondJmp = 11, kAlloc = 12,
        kCondExecPredClean = 13, kCondExecPredCleanEnd = 14, kMarkVsFetchDone = 15
    };

    bool IsEnd(uint32_t op)
    {
        return op == kExecEnd || op == kCondExecEnd || op == kCondExecPredEnd || op == kCondExecPredCleanEnd;
    }

    const char* kPrelude = R"(
cbuffer Sys : register(b0)
{
    float4 vpScale;
    float4 vpOffset;
    float4 rtSize;      // w, h, xyFmt, zFmt
    uint4  sysFlags;    // x: w0Fmt, y: alphaFunc (0 = none), z: gammaOut, w: unused
    float4 alphaRef;
    uint4  boolConst[2];
    uint4  loopConst[8];
    uint4  texSwz[4];
    uint4  texFlags[4];
    uint4  vfEndian[24];
};
cbuffer Consts : register(b1) { float4 C[256]; };

static float4 R[128];
static bool P = false;
static int A0 = 0;
static int AL = 0;
static float PS_ = 0.0;
static float4 oPos = float4(0, 0, 0, 1);
static float4 oI[16];
static float4 oPtSize = float4(0, 0, 0, 0);
static float4 oC[4];
static float oDepth = 0.0;
static bool killed = false;
static uint VI = 0;

uint Sel4(uint4 v, uint w) { return w == 0 ? v.x : w == 1 ? v.y : w == 2 ? v.z : v.w; }
bool BoolC(uint i) { return ((Sel4(boolConst[i >> 7], (i >> 5) & 3) >> (i & 31)) & 1) != 0; }
uint LoopC(uint i) { return Sel4(loopConst[i >> 2], i & 3); }
float Rcp(float x) { return 1.0 / x; }
float ClampInf(float x) { return clamp(x, -3.4028235e38, 3.4028235e38); }
float ZeroInf(float x) { return isfinite(x) ? x : 0.0; }
float Comp(float4 v, uint s) { return s == 0 ? v.x : s == 1 ? v.y : s == 2 ? v.z : s == 3 ? v.w : s == 4 ? 0.0 : 1.0; }
float4 Swz4(float4 v, uint s) { return float4(Comp(v, s & 7), Comp(v, (s >> 3) & 7), Comp(v, (s >> 6) & 7), Comp(v, (s >> 9) & 7)); }
float4 SelSat(bool c, float4 a, float4 b) { return c ? a : b; }
float4 CubeOp(float4 a, float4 b)
{
    float3 v = float3(a.z, a.w, a.x);
    float ax = abs(v.x), ay = abs(v.y), az = abs(v.z);
    float sc, tc, m, face;
    if (az >= ax && az >= ay) { face = v.z < 0 ? 5 : 4; sc = v.z < 0 ? -v.x : v.x; tc = -v.y; m = az; }
    else if (ay >= ax)        { face = v.y < 0 ? 3 : 2; sc = v.x; tc = v.y < 0 ? -v.z : v.z; m = ay; }
    else                      { face = v.x < 0 ? 1 : 0; sc = v.x < 0 ? v.z : -v.z; tc = -v.y; m = ax; }
    return float4(tc, sc, 2.0 * m, face);
}
)";

    const char* kVsPrelude = R"(
uint Swap32(uint L, uint e)
{
    if (e == 2) return ((L & 0xFFu) << 24) | ((L & 0xFF00u) << 8) | ((L >> 8) & 0xFF00u) | (L >> 24);
    if (e == 1) return ((L >> 8) & 0x00FF00FFu) | ((L << 8) & 0xFF00FF00u);
    if (e == 3) return (L >> 16) | (L << 16);
    return L;
}
int SExt(uint v, uint bits) { int s = 32 - (int)bits; return ((int)(v << s)) >> s; }
float2 Unpack16(uint v, bool isInt, bool sgn)
{
    float2 r;
    if (sgn) { r = float2(SExt(v >> 16, 16), SExt(v & 0xFFFF, 16)); if (!isInt) r = max(r / 32767.0, -1.0); }
    else     { r = float2(v >> 16, v & 0xFFFF); if (!isInt) r /= 65535.0; }
    return r;
}
float4 VFetch(ByteAddressBuffer b, uint addr, uint e, uint fmt, bool isInt, bool sgn)
{
    float4 r = float4(0, 0, 0, 1);
    uint v0 = Swap32(b.Load(addr), e);
    if (fmt == 6)
    {
        if (sgn) { r = float4(SExt(v0 >> 24, 8), SExt((v0 >> 16) & 255, 8), SExt((v0 >> 8) & 255, 8), SExt(v0 & 255, 8)); if (!isInt) r = max(r / 127.0, -1.0); }
        else     { r = float4(v0 >> 24, (v0 >> 16) & 255, (v0 >> 8) & 255, v0 & 255); if (!isInt) r /= 255.0; }
    }
    else if (fmt == 7)
    {
        if (sgn) { r = float4(SExt(v0 & 1023, 10), SExt((v0 >> 10) & 1023, 10), SExt((v0 >> 20) & 1023, 10), SExt(v0 >> 30, 2)); if (!isInt) r = max(r / float4(511, 511, 511, 1), -1.0); }
        else     { r = float4(v0 & 1023, (v0 >> 10) & 1023, (v0 >> 20) & 1023, v0 >> 30); if (!isInt) r /= float4(1023, 1023, 1023, 3); }
    }
    else if (fmt == 25) { r.xy = Unpack16(v0, isInt, sgn); }
    else if (fmt == 26) { uint v1 = Swap32(b.Load(addr + 4), e); r.xy = Unpack16(v0, isInt, sgn); r.zw = Unpack16(v1, isInt, sgn); }
    else if (fmt == 31) { r.xy = float2(f16tof32(v0 >> 16), f16tof32(v0 & 0xFFFF)); }
    else if (fmt == 32) { uint v1 = Swap32(b.Load(addr + 4), e); r = float4(f16tof32(v0 >> 16), f16tof32(v0 & 0xFFFF), f16tof32(v1 >> 16), f16tof32(v1 & 0xFFFF)); }
    else if (fmt == 33) { r.x = sgn ? (float)(int)v0 : (float)v0; }
    else if (fmt == 34) { uint v1 = Swap32(b.Load(addr + 4), e); r.xy = sgn ? float2((int)v0, (int)v1) : float2(v0, v1); }
    else if (fmt == 35) { uint v1 = Swap32(b.Load(addr + 4), e), v2 = Swap32(b.Load(addr + 8), e), v3 = Swap32(b.Load(addr + 12), e); r = sgn ? float4((int)v0, (int)v1, (int)v2, (int)v3) : float4(v0, v1, v2, v3); }
    else if (fmt == 36) { r.x = asfloat(v0); }
    else if (fmt == 37) { r.xy = float2(asfloat(v0), asfloat(Swap32(b.Load(addr + 4), e))); }
    else if (fmt == 38) { r = float4(asfloat(v0), asfloat(Swap32(b.Load(addr + 4), e)), asfloat(Swap32(b.Load(addr + 8), e)), asfloat(Swap32(b.Load(addr + 12), e))); }
    else if (fmt == 57) { r = float4(asfloat(v0), asfloat(Swap32(b.Load(addr + 4), e)), asfloat(Swap32(b.Load(addr + 8), e)), 1.0); }
    return r;
}
StructuredBuffer<uint4> vtxList : register(t120);
)";

    struct Translator
    {
        bool vertex;
        const uint32_t* code;
        uint32_t count;
        XenosShader& out;
        std::string body;
        std::string err;
        int indent = 1;
        int flowDepth = 0;           // inside conditional flow: no implicit-derivative sampling
        uint32_t maxReg = 0;
        // last full vertex fetch (for mini fetches)
        uint32_t lastSlot = 0, lastStride = 0;
        std::vector<CfInstr> cf;

        Translator(bool v, const uint32_t* c, uint32_t n, XenosShader& o) : vertex(v), code(c), count(n), out(o) {}

        void Line(const std::string& s)
        {
            body.append(static_cast<size_t>(indent) * 2, ' ');
            body += s;
            body += '\n';
        }

        static std::string Fmt(const char* f, ...)
        {
            char buf[1024];
            va_list ap;
            va_start(ap, f);
            vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        // ---- ALU ----------------------------------------------------------------------------
        static const char* Comp(uint32_t i) { static const char* c[] = { "x", "y", "z", "w" }; return c[i & 3]; }

        std::string Swizzle(uint32_t swz)
        {
            std::string s = ".";
            for (uint32_t i = 0; i < 4; i++)
                s += Comp(((swz >> (2 * i)) + i) & 3);
            return s;
        }

        // Source operand `i` (1..3) as a float4 expression.
        std::string Source(const uint32_t* d, uint32_t i, int& constCount)
        {
            const uint32_t reg = (d[2] >> (8 * (3 - i))) & 0xFF;      // src1 at bits 16-23, src2 8-15, src3 0-7
            const bool isTemp = (d[2] >> (31 - (i - 1))) & 1;          // src1_sel bit31, src2_sel 30, src3_sel 29
            const uint32_t swz = (d[1] >> (8 * (3 - i))) & 0xFF;       // src1 16-23, src2 8-15, src3 0-7
            const bool neg = (d[1] >> (24 + (3 - i))) & 1;            // src3 24, src2 25, src1 26
            std::string e;
            if (isTemp)
            {
                const uint32_t r = reg & 0x3F;
                if (r + 1 > maxReg) maxReg = r + 1;
                e = (reg & 0x40) ? Fmt("R[%u + AL]", r) : Fmt("R[%u]", r);
                if (reg & 0x80) e = "abs(" + e + ")";
            }
            else
            {
                const bool addressed = (constCount == 0 ? (d[1] >> 31) : (d[1] >> 30)) & 1;
                constCount++;
                if (addressed)
                    e = Fmt("C[%u + %s]", reg, ((d[1] >> 29) & 1) ? "A0" : "AL");
                else
                    e = Fmt("C[%u]", reg);
                if ((d[0] >> 7) & 1) e = "abs(" + e + ")";
            }
            e += Swizzle(swz);
            if (neg) e = "(-" + e + ")";
            return e;
        }

        std::string ExportName(bool isVsExport, uint32_t reg, bool& ok)
        {
            ok = true;
            if (vertex)
            {
                if (reg < 16) { out.exportMask |= 1u << reg; return Fmt("oI[%u]", reg); }
                if (reg == 62) return "oPos";
                if (reg == 63) return "oPtSize";
            }
            else
            {
                if (reg < 4) return Fmt("oC[%u]", reg);
                if (reg == 61) { out.writesDepth = true; return "oDepth"; }
            }
            ok = false;
            return "";
        }

        void WriteDest(const std::string& dest, uint32_t mask, const std::string& src /*float4 expr name*/)
        {
            static const char* c[] = { "x", "y", "z", "w" };
            for (uint32_t k = 0; k < 4; k++)
                if (mask & (1u << k))
                    Line(Fmt("%s.%s = %s.%s;", dest.c_str(), c[k], src.c_str(), c[k]));
        }

        void EmitAlu(const uint32_t* d)
        {
            Line(Fmt("// alu %08X %08X %08X", d[0], d[1], d[2]));
            const bool predicated = (d[1] >> 28) & 1;
            const bool predCond = (d[1] >> 27) & 1;
            if (predicated)
            {
                Line(Fmt("if (P == %s) {", predCond ? "true" : "false"));
                indent++;
            }

            const uint32_t vOp = (d[2] >> 24) & 0x1F;
            const uint32_t sOp = (d[0] >> 26) & 0x3F;
            const uint32_t vMask = (d[0] >> 16) & 0xF;
            const uint32_t sMask = (d[0] >> 20) & 0xF;
            const bool exportData = (d[0] >> 15) & 1;
            const uint32_t vDest = d[0] & 0x3F;
            const bool vDestRel = (d[0] >> 6) & 1;
            const uint32_t sDest = (d[0] >> 8) & 0x3F;
            const bool sDestRel = (d[0] >> 14) & 1;
            const bool vClamp = (d[0] >> 24) & 1;
            const bool sClamp = (d[0] >> 25) & 1;

            // Operand setup (vector: src1..3 = a,b,c; scalar: src3).
            int cc = 0;
            Line("{");
            indent++;
            Line("float4 a = " + Source(d, 1, cc) + ";");
            Line("float4 b = " + Source(d, 2, cc) + ";");
            Line("float4 c = " + Source(d, 3, cc) + ";");
            Line("float4 vr = float4(0,0,0,0);");
            Line("float sr = 0.0;");
            Line("float sa = c.w, sb = c.x;");

            // Vector operation.
            switch (vOp)
            {
            case 0: Line("vr = a + b;"); break;
            case 1: Line("vr = a * b;"); break;
            case 2: Line("vr = max(a, b);"); break;
            case 3: Line("vr = min(a, b);"); break;
            case 4: Line("vr = (float4)(a == b);"); break;
            case 5: Line("vr = (float4)(a > b);"); break;
            case 6: Line("vr = (float4)(a >= b);"); break;
            case 7: Line("vr = (float4)(a != b);"); break;
            case 8: Line("vr = frac(a);"); break;
            case 9: Line("vr = trunc(a);"); break;
            case 10: Line("vr = floor(a);"); break;
            case 11: Line("vr = a * b + c;"); break;
            case 12: Line("vr = (a == 0.0) ? b : c;"); break;
            case 13: Line("vr = (a >= 0.0) ? b : c;"); break;
            case 14: Line("vr = (a > 0.0) ? b : c;"); break;
            case 15: Line("vr = dot(a, b).xxxx;"); break;
            case 16: Line("vr = dot(a.xyz, b.xyz).xxxx;"); break;
            case 17: Line("vr = (a.x * b.x + a.y * b.y + c.x).xxxx;"); break;
            case 18: Line("vr = CubeOp(a, b);"); break;
            case 19: Line("vr = max(max(a.x, a.y), max(a.z, a.w)).xxxx;"); break;
            case 20: case 21: case 22: case 23:
            {
                static const char* cmp[] = { "== 0.0", "!= 0.0", "> 0.0", ">= 0.0" };
                Line(Fmt("P = (a.w == 0.0 && b.w %s);", cmp[vOp - 20]));
                Line(Fmt("vr = (float4)((a.x == 0.0 && b.x %s) ? 0.0 : a.x + 1.0);", cmp[vOp - 20]));
                break;
            }
            case 24: case 25: case 26: case 27:
            {
                static const char* cmp[] = { "==", ">", ">=", "!=" };
                Line(Fmt("{ bool4 k = (a %s b); vr = (float4)k; if (any(k)) killed = true; }", cmp[vOp - 24]));
                out.usesKill = true;
                break;
            }
            case 28: Line("vr = float4(1.0, a.y * b.y, a.z, b.w);"); break;
            case 29: Line("A0 = (int)clamp(floor(a.w + 0.5), -256.0, 255.0); vr = max(a, b);"); break;
            default: err += Fmt("unhandled vector op %u; ", vOp); break;
            }
            if (vClamp) Line("vr = saturate(vr);");

            // Scalar operation.
            switch (sOp)
            {
            case 0: Line("sr = sa + sb;"); break;
            case 1: Line("sr = sa + PS_;"); break;
            case 2: Line("sr = sa * sb;"); break;
            case 3: Line("sr = sa * PS_;"); break;
            case 4: Line("sr = (PS_ == -3.4028235e38 || !isfinite(PS_) || !isfinite(sb) || sb <= 0.0) ? -3.4028235e38 : sa * PS_;"); break;
            case 5: Line("sr = max(sa, sb);"); break;
            case 6: Line("sr = min(sa, sb);"); break;
            case 7: Line("sr = (sa == 0.0) ? 1.0 : 0.0;"); break;
            case 8: Line("sr = (sa > 0.0) ? 1.0 : 0.0;"); break;
            case 9: Line("sr = (sa >= 0.0) ? 1.0 : 0.0;"); break;
            case 10: Line("sr = (sa != 0.0) ? 1.0 : 0.0;"); break;
            case 11: Line("sr = frac(sa);"); break;
            case 12: Line("sr = trunc(sa);"); break;
            case 13: Line("sr = floor(sa);"); break;
            case 14: Line("sr = exp2(sa);"); break;
            case 15: Line("sr = ClampInf(log2(sa));"); break;
            case 16: Line("sr = log2(sa);"); break;
            case 17: Line("sr = ClampInf(Rcp(sa));"); break;
            case 18: Line("sr = ZeroInf(Rcp(sa));"); break;
            case 19: Line("sr = Rcp(sa);"); break;
            case 20: Line("sr = ClampInf(rsqrt(sa));"); break;
            case 21: Line("sr = ZeroInf(rsqrt(sa));"); break;
            case 22: Line("sr = rsqrt(sa);"); break;
            case 23: Line("A0 = (int)clamp(floor(sa + 0.5), -256.0, 255.0); sr = max(sa, sb);"); break;
            case 24: Line("A0 = (int)clamp(floor(sa), -256.0, 255.0); sr = max(sa, sb);"); break;
            case 25: Line("sr = sa - sb;"); break;
            case 26: Line("sr = sa - PS_;"); break;
            case 27: Line("P = (sa == 0.0); sr = P ? 0.0 : 1.0;"); break;
            case 28: Line("P = (sa != 0.0); sr = P ? 0.0 : 1.0;"); break;
            case 29: Line("P = (sa > 0.0); sr = P ? 0.0 : 1.0;"); break;
            case 30: Line("P = (sa >= 0.0); sr = P ? 0.0 : 1.0;"); break;
            case 31: Line("if (sa == 1.0) { sr = 0.0; P = true; } else { sr = (sa == 0.0) ? 1.0 : sa; P = false; }"); break;
            case 32: Line("if (sa - 1.0 <= 0.0) { sr = 0.0; P = true; } else { sr = sa - 1.0; P = false; }"); break;
            case 33: Line("sr = 3.4028235e38; P = false;"); break;
            case 34: Line("if (sa == 0.0) { sr = 0.0; P = true; } else { sr = sa; P = false; }"); break;
            case 35: Line("if (sa == 0.0) { sr = 1.0; killed = true; }"); out.usesKill = true; break;
            case 36: Line("if (sa > 0.0) { sr = 1.0; killed = true; }"); out.usesKill = true; break;
            case 37: Line("if (sa >= 0.0) { sr = 1.0; killed = true; }"); out.usesKill = true; break;
            case 38: Line("if (sa != 0.0) { sr = 1.0; killed = true; }"); out.usesKill = true; break;
            case 39: Line("if (sa == 1.0) { sr = 1.0; killed = true; }"); out.usesKill = true; break;
            case 40: Line("sr = sqrt(sa);"); break;
            case 42: case 43: case 44: case 45: case 46: case 47:
            {
                // const (src3 as c#.w) op temp register (r#.x)
                const uint32_t treg = (sOp & 1) | (((d[2] >> 29) & 1) << 1) | ((d[1]) & 0x3C);
                if (treg + 1 > maxReg) maxReg = treg + 1;
                const char* op = sOp < 44 ? "*" : (sOp < 46 ? "+" : "-");
                {
                    const uint32_t cidx = d[2] & 0xFF;
                    const bool addressed = (d[1] >> 31) & 1; // const_0_rel_abs (src1/src2 are registers here)
                    const uint32_t ca = (((d[1] >> 6) & 3) + 3) & 3, tb = (d[1] & 3);
                    const std::string cexpr = addressed ? Fmt("C[%u + %s]", cidx, ((d[1] >> 29) & 1) ? "A0" : "AL") : Fmt("C[%u]", cidx);
                    Line(Fmt("sr = %s.%s %s R[%u].%s;", cexpr.c_str(), Comp(ca), op, treg, Comp(tb)));
                }
                break;
            }
            case 48: Line("sr = sin(sa);"); break;
            case 49: Line("sr = cos(sa);"); break;
            case 50: Line("sr = PS_;"); break;
            default: err += Fmt("unhandled scalar op %u; ", sOp); break;
            }
            if (sClamp) Line("sr = saturate(sr);");

            // Write-back (both results computed from the old state).
            std::string vdst, sdst;
            bool ok = true;
            if (exportData)
            {
                // Both operations export to vector_dest; masks combine per component:
                // vector only -> vector result, scalar only -> scalar result, both -> constant 1,
                // neither (and scalar_dest_rel) -> constant 0.
                vdst = ExportName(true, vDest, ok);
                if (!ok)
                {
                    if (vMask | sMask) err += Fmt("unknown export %u; ", vDest);
                }
                else
                {
                    const uint32_t vOnly = vMask & ~sMask, sOnly = sMask & ~vMask, one = vMask & sMask;
                    const uint32_t zero = sDestRel ? (~(vMask | sMask) & 0xF) : 0;
                    if (vOnly) WriteDest(vdst, vOnly, "vr");
                    if (sOnly)
                    {
                        Line("float4 sv = float4(sr, sr, sr, sr);");
                        WriteDest(vdst, sOnly, "sv");
                    }
                    static const char* cc[] = { "x", "y", "z", "w" };
                    for (uint32_t k = 0; k < 4; k++)
                    {
                        if (one & (1u << k)) Line(Fmt("%s.%s = 1.0;", vdst.c_str(), cc[k]));
                        if (zero & (1u << k)) Line(Fmt("%s.%s = 0.0;", vdst.c_str(), cc[k]));
                    }
                }
            }
            else
            {
                if (vDest + 1 > maxReg) maxReg = vDest + 1;
                if (sDest + 1 > maxReg) maxReg = sDest + 1;
                vdst = vDestRel ? Fmt("R[%u + AL]", vDest) : Fmt("R[%u]", vDest);
                sdst = sDestRel ? Fmt("R[%u + AL]", sDest) : Fmt("R[%u]", sDest);
                if (vMask) WriteDest(vdst, vMask, "vr");
                if (sMask)
                {
                    Line("float4 sv = float4(sr, sr, sr, sr);");
                    WriteDest(sdst, sMask, "sv");
                }
            }
            Line("PS_ = sr;");
            indent--;
            Line("}");

            if (predicated)
            {
                indent--;
                Line("}");
            }
        }

        // ---- fetch ------------------------------------------------------------------------------
        void EmitFetch(const uint32_t* d)
        {
            Line(Fmt("// fetch %08X %08X %08X", d[0], d[1], d[2]));
            const uint32_t opcode = d[0] & 0x1F;
            const bool predicated = (d[1] >> 31) & 1;
            const bool predCond = (d[2] >> 31) & 1;
            if (predicated)
            {
                Line(Fmt("if (P == %s) {", predCond ? "true" : "false"));
                indent++;
            }
            const uint32_t srcReg = (d[0] >> 5) & 0x3F;
            const bool srcRel = (d[0] >> 11) & 1;
            const uint32_t dstReg = (d[0] >> 12) & 0x3F;
            const bool dstRel = (d[0] >> 18) & 1;
            const std::string src = srcRel ? Fmt("R[%u + AL]", srcReg) : Fmt("R[%u]", srcReg);
            const std::string dst = dstRel ? Fmt("R[%u + AL]", dstReg) : Fmt("R[%u]", dstReg);
            if (srcReg + 1 > maxReg) maxReg = srcReg + 1;
            if (dstReg + 1 > maxReg) maxReg = dstReg + 1;
            const uint32_t dstSwz = d[1] & 0xFFF;

            auto writeResult = [&](const char* var)
            {
                static const char* c[] = { "x", "y", "z", "w" };
                for (uint32_t k = 0; k < 4; k++)
                {
                    const uint32_t s = (dstSwz >> (3 * k)) & 7;
                    if (s == 7)
                        continue;
                    if (s <= 3)
                        Line(Fmt("%s.%s = %s.%s;", dst.c_str(), c[k], var, c[s]));
                    else
                        Line(Fmt("%s.%s = %s;", dst.c_str(), c[k], s == 4 ? "0.0" : "1.0"));
                }
            };

            if (opcode == 0) // vertex fetch
            {
                if (!vertex)
                    err += "vfetch in pixel shader; ";
                const bool mini = (d[1] >> 30) & 1;
                uint32_t slot = lastSlot, stride = lastStride;
                if (!mini)
                {
                    const uint32_t constIndex = (d[0] >> 20) & 0x1F;
                    const uint32_t sel = (d[0] >> 25) & 3;
                    slot = constIndex * 3 + sel;
                    stride = d[2] & 0xFF;
                    lastSlot = slot;
                    lastStride = stride;
                }
                int32_t offsetS = static_cast<int32_t>((d[2] >> 8) & 0x7FFFFF);
                if (offsetS & 0x400000) offsetS -= 0x800000;
                const uint32_t offset = offsetS < 0 ? 0u : static_cast<uint32_t>(offsetS);
                const uint32_t fmt = (d[1] >> 16) & 0x3F;
                static const int vfOff = std::getenv("Q4_VFOFF") ? std::atoi(std::getenv("Q4_VFOFF")) : 0;
                if (fmt == 37) offsetS += vfOff;
                const bool isInt = (d[1] >> 13) & 1;
                const bool sgn = (d[1] >> 14) & 1;
                int32_t expAdj = static_cast<int32_t>((d[1] >> 24) & 0x3F);
                if (expAdj & 0x20) expAdj -= 64;
                if (slot >= kVfetchSlots) { err += "vfetch slot out of range; "; slot = 0; }
                out.vfetchSlots[slot >> 6] |= 1ull << (slot & 63);
                out.slotStride[slot] = stride ? stride : 1;
                if (offset + 4 > out.slotExtent[slot]) out.slotExtent[slot] = offset + 4;

                const uint32_t srcComp = (d[0] >> 30) & 3;
                Line("{");
                indent++;
                if (!mini)
                {
                    const bool rounded = (d[1] >> 15) & 1;
                    Line(Fmt("VI = (uint)max(floor(%s.%s%s), 0.0);", src.c_str(), Comp(srcComp), rounded ? " + 0.5" : ""));
                }
                Line(Fmt("float4 f = VFetch(vb%u, (uint)((int)(%d) + (int)VI * %u) * 4, vfEndian[%u][%u], %u, %s, %s);", slot, (int)offsetS,
                         stride ? stride : 1, slot >> 2, slot & 3, fmt, isInt ? "true" : "false", sgn ? "true" : "false"));
                if (expAdj)
                    Line(Fmt("f *= exp2(%d.0);", expAdj));
                writeResult("f");
                indent--;
                Line("}");
            }
            else if (opcode == 1) // texture fetch
            {
                const uint32_t constIndex = (d[0] >> 20) & 0x1F;
                const bool denorm = (d[0] >> 25) & 1;
                const uint32_t srcSwz = (d[0] >> 26) & 0x3F;
                const uint32_t dim = (d[2] >> 14) & 3;
                if (vertex)
                {
                    err += "texture fetch in vertex shader; ";
                }
                else if (constIndex < 16)
                {
                    if (dim == 2) out.texMask3D |= 1u << constIndex;
                    else if (dim == 3) out.texMaskCube |= 1u << constIndex;
                    else out.texMask2D |= 1u << constIndex;
                    Line("{");
                    indent++;
                    Line(Fmt("float3 uvw = float3(%s.%s, %s.%s, %s.%s);", src.c_str(), Comp(srcSwz & 3), src.c_str(),
                             Comp((srcSwz >> 2) & 3), src.c_str(), Comp((srcSwz >> 4) & 3)));
                    Line("float4 t;");
                    if (dim == 2)
                    {
                        Line(Fmt("t = tex3_%u.SampleLevel(smp%u, uvw, 0);", constIndex, constIndex));
                    }
                    else if (dim == 3)
                    {
                        // (sc, tc, face) -> direction
                        Line("float3 dir; { float s = uvw.x * 2.0 - 1.0, t2 = uvw.y * 2.0 - 1.0; uint f = (uint)uvw.z;"
                             " dir = f == 0 ? float3(1, -t2, -s) : f == 1 ? float3(-1, -t2, s) : f == 2 ? float3(s, 1, t2) :"
                             " f == 3 ? float3(s, -1, -t2) : f == 4 ? float3(s, -t2, 1) : float3(-s, -t2, -1); }");
                        Line(Fmt("t = texc_%u.SampleLevel(smp%u, dir, 0);", constIndex, constIndex));
                    }
                    else
                    {
                        if (denorm)
                        {
                            Line(Fmt("{ float w, h; tex2_%u.GetDimensions(w, h); uvw.xy /= float2(w, h); }", constIndex));
                        }
                        if (flowDepth == 0)
                            Line(Fmt("t = tex2_%u.Sample(smp%u, uvw.xy);", constIndex, constIndex));
                        else
                            Line(Fmt("t = tex2_%u.SampleLevel(smp%u, uvw.xy, 0);", constIndex, constIndex));
                    }
                    Line(Fmt("t = Swz4(t, (texSwz[%u][%u]));", constIndex >> 2, constIndex & 3));
                    writeResult("t");
                    indent--;
                    Line("}");
                }
            }
            else
            {
                // getTextureGradients / setTextureLOD / etc: not handled; leave the destination alone.
                err += Fmt("unhandled fetch opcode %u; ", opcode);
            }

            if (predicated)
            {
                indent--;
                Line("}");
            }
        }

        void EmitExecBody(const CfInstr& c)
        {
            const uint32_t base = c.execAddress();
            for (uint32_t k = 0; k < c.execCount(); k++)
            {
                const uint32_t addr = (base + k) * 3;
                if (addr + 3 > count)
                {
                    err += "exec past end; ";
                    return;
                }
                const bool isFetch = (c.sequence() >> (2 * k)) & 1;
                if (isFetch)
                    EmitFetch(code + addr);
                else
                    EmitAlu(code + addr);
            }
        }

        bool NeedsStateMachine() const
        {
            for (const CfInstr& c : cf)
            {
                const uint32_t op = c.opcode();
                if (op == kCondCall || op == kReturn || op == kCondJmp)
                    return true;
            }
            return false;
        }

        // General (slower) form: every control-flow instruction is a case of a program-counter loop.
        bool EmitStateMachine()
        {
            Line("uint pc = 0; uint callStack[8]; uint sp = 0; uint lcnt[32]; int lstart[32]; int lstep[32]; uint lit[32]; int lsave[32];");
            Line("[loop] while (true) {");
            indent++;
                        flowDepth++;
            for (uint32_t i = 0; i < cf.size(); i++)
            {
                const CfInstr& c = cf[i];
                const uint32_t op = c.opcode();
                Line(Fmt("if (pc == %u) {", i));
                indent++;
                switch (op)
                {
                case kNop: case kAlloc: case kMarkVsFetchDone:
                    break;
                case kExec: case kExecEnd:
                    EmitExecBody(c);
                    if (op == kExecEnd) Line("return;");
                    break;
                case kCondExec: case kCondExecEnd:
                    Line(Fmt("if (%sBoolC(%u)) {", c.condition() ? "" : "!", c.boolAddress()));
                    indent++;
                    EmitExecBody(c);
                    indent--;
                    Line("}");
                    if (op == kCondExecEnd) Line("return;");
                    break;
                case kCondExecPred: case kCondExecPredEnd: case kCondExecPredClean: case kCondExecPredCleanEnd:
                    Line(Fmt("if (P == %s) {", c.condition() ? "true" : "false"));
                    indent++;
                    EmitExecBody(c);
                    indent--;
                    Line("}");
                    if (op == kCondExecPredClean || op == kCondExecPredCleanEnd) Line("P = false;");
                    if (op == kCondExecPredEnd || op == kCondExecPredCleanEnd) Line("return;");
                    break;
                case kLoopStart:
                    Line(Fmt("{ uint lc = LoopC(%u); lcnt[%u] = lc & 255; lstart[%u] = (int)((lc >> 8) & 255); "
                             "lstep[%u] = ((int)((lc >> 16) << 24)) >> 24; lit[%u] = 0; lsave[%u] = AL;", c.loopId(), c.loopId(),
                             c.loopId(), c.loopId(), c.loopId(), c.loopId()));
                    Line(Fmt("  if (lcnt[%u] == 0) { pc = %u; continue; } AL = lstart[%u]; }", c.loopId(), c.jumpAddress(), c.loopId()));
                    break;
                case kLoopEnd:
                {
                    const uint32_t id = c.loopId();
                    if (c.predicatedBreak())
                        Line(Fmt("if (P == %s) { AL = lsave[%u]; pc = %u; continue; }", c.condition() ? "true" : "false", id, i + 1));
                    Line(Fmt("lit[%u]++;", id));
                    Line(Fmt("if (lit[%u] < lcnt[%u]) { AL = lstart[%u] + (int)lit[%u] * lstep[%u]; pc = %u; continue; }", id, id, id, id,
                             id, c.jumpAddress()));
                    Line(Fmt("AL = lsave[%u];", id));
                    break;
                }
                case kCondCall:
                {
                    const bool uncond = (c.w0 >> 13) & 1, pred = (c.w0 >> 14) & 1;
                    std::string cond = uncond ? "true" : pred ? Fmt("(P == %s)", c.condition() ? "true" : "false")
                                                              : Fmt("(%sBoolC(%u))", c.condition() ? "" : "!", c.boolAddress());
                    Line(Fmt("if (%s) { if (sp < 8) callStack[sp++] = %u; pc = %u; continue; }", cond.c_str(), i + 1, c.jumpAddress()));
                    break;
                }
                case kReturn:
                    Line(Fmt("if (sp > 0) { pc = callStack[--sp]; continue; } return;"));
                    break;
                case kCondJmp:
                {
                    const bool uncond = (c.w0 >> 13) & 1, pred = (c.w0 >> 14) & 1;
                    std::string cond = uncond ? "true" : pred ? Fmt("(P == %s)", c.condition() ? "true" : "false")
                                                              : Fmt("(%sBoolC(%u))", c.condition() ? "" : "!", c.boolAddress());
                    Line(Fmt("if (%s) { pc = %u; continue; }", cond.c_str(), c.jumpAddress()));
                    break;
                }
                default:
                    err += Fmt("unsupported control flow opcode %u; ", op);
                    break;
                }
                Line(Fmt("pc = %u; continue;", i + 1));
                indent--;
                Line("}");
            }
            Line("return;");
            flowDepth--;
            indent--;
            Line("}");
            return true;
        }

        // Emits control flow for cf[from..to).
        bool EmitFlow(uint32_t from, uint32_t to)
        {
            for (uint32_t i = from; i < to; i++)
            {
                const CfInstr& c = cf[i];
                const uint32_t op = c.opcode();
                switch (op)
                {
                case kNop: case kAlloc: case kMarkVsFetchDone:
                    break;
                case kExec: case kExecEnd:
                    Line("{");
                    indent++;
                    EmitExecBody(c);
                    indent--;
                    Line("}");
                    if (op == kExecEnd) { Line("return;"); return true; }
                    break;
                case kCondExec: case kCondExecEnd:
                    Line(Fmt("if (%sBoolC(%u)) {", c.condition() ? "" : "!", c.boolAddress()));
                    indent++;
                    flowDepth++;
                    EmitExecBody(c);
                    flowDepth--;
                    indent--;
                    Line("}");
                    if (op == kCondExecEnd) { Line("return;"); return true; }
                    break;
                case kCondExecPred: case kCondExecPredEnd: case kCondExecPredClean: case kCondExecPredCleanEnd:
                    Line(Fmt("if (P == %s) {", c.condition() ? "true" : "false"));
                    indent++;
                    flowDepth++;
                    EmitExecBody(c);
                    flowDepth--;
                    indent--;
                    Line("}");
                    if (op == kCondExecPredClean || op == kCondExecPredCleanEnd)
                        Line("P = false;");
                    if (op == kCondExecPredEnd || op == kCondExecPredCleanEnd) { Line("return;"); return true; }
                    break;
                case kLoopStart:
                {
                    // address: the instruction after the matching loop_end.
                    const uint32_t after = c.jumpAddress();
                    if (after == 0 || after > cf.size() || after <= i + 1)
                    {
                        err += "bad loop; ";
                        return false;
                    }
                    Line("{");
                    indent++;
                    Line(Fmt("uint lc = LoopC(%u); int aLsave = AL; uint cnt = lc & 255; int aStart = (int)((lc >> 8) & 255); "
                             "int aStep = ((int)((lc >> 16) << 24)) >> 24;", c.loopId()));
                    Line("[loop] for (uint it = 0; it < cnt; it++) {");
                    indent++;
                    Line("AL = aStart + (int)it * aStep;");
                    flowDepth++;
                    if (!EmitFlow(i + 1, after - 1))
                        return false;
                    // matching loop_end (cf[after-1]): predicated break
                    const CfInstr& le = cf[after - 1];
                    if (le.opcode() == kLoopEnd && le.predicatedBreak())
                        Line(Fmt("if (P == %s) break;", le.condition() ? "true" : "false"));
                    flowDepth--;
                    indent--;
                    Line("}");
                    Line("AL = aLsave;");
                    indent--;
                    Line("}");
                    i = after - 1;
                    break;
                }
                default:
                    err += Fmt("unsupported control flow opcode %u at %u; ", op, i);
                    return false;
                }
            }
            return true;
        }
    };
}

bool TranslateXenosShader(bool vertex, const uint32_t* dwords, uint32_t count, XenosShader& out)
{
    out = XenosShader{};
    out.vertex = vertex;
    Translator t(vertex, dwords, count, out);

    // Control flow section: instructions until the first end (the exec addresses start after it).
    uint32_t minExec = count / 3;
    for (uint32_t i = 0; i < 512 && (i / 2) * 3 + 3 <= count; i++)
    {
        const uint32_t* d = dwords + (i / 2) * 3;
        CfInstr c;
        if ((i & 1) == 0)
        {
            c.w0 = d[0];
            c.w1 = d[1] & 0xFFFF;
        }
        else
        {
            c.w0 = (d[1] >> 16) | (d[2] << 16);
            c.w1 = d[2] >> 16;
        }
        t.cf.push_back(c);
        const uint32_t op = c.opcode();
        if (((op >= kExec && op <= kCondExecPredEnd) || op == kCondExecPredClean || op == kCondExecPredCleanEnd) && c.execAddress() < minExec)
            minExec = c.execAddress();
        if (op == kExec || op == kCondExec || op == kCondExecPred || op == kCondExecPredClean)
        {
        }
        // The control-flow section ends where the executable section starts.
        if (i / 2 + 1 >= minExec)
            break;
    }
    // Trim anything that overlaps the exec section.
    while (!t.cf.empty() && ((t.cf.size() - 1) / 2) >= minExec)
        t.cf.pop_back();

    std::string main;
    t.indent = 1;
    const bool flowOk = t.NeedsStateMachine() ? t.EmitStateMachine() : t.EmitFlow(0, static_cast<uint32_t>(t.cf.size()));
    if (!flowOk)
    {
        out.error = t.err.empty() ? "control flow" : t.err;
        return false;
    }
    if (!t.err.empty())
    {
        out.error = t.err;
        // Unhandled instructions are skipped (translation continues) but reported.
    }

    std::string src;
    src += kPrelude;
    if (vertex)
    {
        src += kVsPrelude;
        for (uint32_t s = 0; s < kVfetchSlots; s++)
            if (out.vfetchSlots[s >> 6] & (1ull << (s & 63)))
                src += Translator::Fmt("ByteAddressBuffer vb%u : register(t%u);\n", s, s);
        src += "void RunVS(uint idx)\n{\n";
        src += "  R[0] = float4((float)idx, 0, 0, 0);\n";
        src += t.body;
        src += "}\n";
        src += R"(
struct VsOut { float4 pos : SV_Position; float4 i[16] : TEXCOORD0; float4 psz : PSIZE_; };
void RunOne(uint idx, out float4 p, out float4 ii[16], out float4 ps)
{
    R[0] = float4(0,0,0,0); P = false; A0 = 0; AL = 0; PS_ = 0.0; oPos = float4(0,0,0,1); oPtSize = float4(0,0,0,0);
    [unroll] for (int k = 0; k < 16; k++) oI[k] = float4(0,0,0,0);
    RunVS(idx);
    p = oPos; ps = oPtSize;
    [unroll] for (int k2 = 0; k2 < 16; k2++) ii[k2] = oI[k2];
}
VsOut main(uint vid : SV_VertexID)
{
    uint4 e = vtxList[vid];
    float4 p, ps; float4 ii[16];
    VsOut o;
    if (e.w == 0) RunOne(e.x, p, ii, ps);
    else if (e.w == 1) RunOne(e.x, p, ii, ps);
    else if (e.w == 2) RunOne(e.y, p, ii, ps);
    else if (e.w == 3) RunOne(e.z, p, ii, ps);
    else
    {
        float4 p1, ps1, p2, ps2, p3, ps3; float4 i1[16], i2[16], i3[16];
        RunOne(e.x, p1, i1, ps1);
        RunOne(e.y, p2, i2, ps2);
        RunOne(e.z, p3, i3, ps3);
        p = p1 + p3 - p2; ps = ps1 + ps3 - ps2;
        [unroll] for (int k = 0; k < 16; k++) ii[k] = i1[k] + i3[k] - i2[k];
    }
    // viewport transform
    float4 q = p;
    float invW = rcp(q.w);
    float2 ndc = (rtSize.z != 0.0) ? q.xy : q.xy * invW;
    float zz = (rtSize.w != 0.0) ? q.z : q.z * invW;
    float2 scr = ndc * vpScale.xy + vpOffset.xy;
    float2 clipXY = float2((scr.x / rtSize.x) * 2.0 - 1.0, 1.0 - (scr.y / rtSize.y) * 2.0);
    float zs = zz * vpScale.z + vpOffset.z;
    float w = (rtSize.z != 0.0 || sysFlags.x != 0) ? 1.0 : q.w;
    o.pos = float4(clipXY * w, zs * w, w);
    [unroll] for (int k = 0; k < 16; k++) o.i[k] = ii[k];
    o.psz = ps;
    return o;
}
)";
    }
    else
    {
        for (uint32_t s = 0; s < 16; s++)
        {
            if (out.texMask2D & (1u << s))
                src += Translator::Fmt("Texture2D<float4> tex2_%u : register(t%u);\n", s, kTex2DBase + s);
            if (out.texMask3D & (1u << s))
                src += Translator::Fmt("Texture3D<float4> tex3_%u : register(t%u);\n", s, kTex3DBase + s);
            if (out.texMaskCube & (1u << s))
                src += Translator::Fmt("TextureCube<float4> texc_%u : register(t%u);\n", s, kTexCubeBase + s);
            src += Translator::Fmt("SamplerState smp%u : register(s%u);\n", s, s);
        }
        src += "void RunPS()\n{\n";
        src += t.body;
        src += "}\n";
        src += R"(
struct PsIn { float4 pos : SV_Position; float4 i[16] : TEXCOORD0; float4 psz : PSIZE_; };
struct PsOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;
               DEPTHMEMBER};
float3 ToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055; }
PsOut main(PsIn pin)
{
    P = false; A0 = 0; AL = 0; PS_ = 0.0;
    [unroll] for (int k = 0; k < 16; k++) R[k] = pin.i[k];
    oC[0] = float4(0,0,0,0); oC[1] = float4(0,0,0,0); oC[2] = float4(0,0,0,0); oC[3] = float4(0,0,0,0);
    oDepth = pin.pos.z;
    RunPS();
    if (killed) discard;
    TEXDBG
    if (sysFlags.w >= 200 && sysFlags.w < 300) { oC[0] = pin.i[(sysFlags.w - 200) & 15]; oC[0].w = 1.0; }
    else if (sysFlags.w != 0) { oC[0] = R[(sysFlags.w - 1) & 127]; oC[0].w = 1.0; }
    uint af = sysFlags.y;
    if (af != 0)
    {
        float a = oC[0].w, r = alphaRef.x;
        bool alphaOk = af == 1 ? a < r : af == 2 ? a == r : af == 3 ? a <= r : af == 4 ? a > r : af == 5 ? a != r : af == 6 ? a >= r : af == 7 ? true : false;
        if (!alphaOk) discard;
    }
    if (sysFlags.z != 0) oC[0].rgb = ToSrgb(saturate(oC[0].rgb));
    PsOut o;
    o.c0 = oC[0]; o.c1 = oC[1]; o.c2 = oC[2]; o.c3 = oC[3];
    DEPTHASSIGN
    return o;
}
)";
    }
    if (!vertex)
    {
        {
            std::string dbg;
            for (uint32_t s = 0; s < 16; s++)
                if (out.texMask2D & (1u << s))
                    dbg += Translator::Fmt("if (sysFlags.w == %u) { oC[0] = tex2_%u.SampleLevel(smp%u, float2(0.37, 0.43), 0); oC[0].w = 1.0; } if (sysFlags.w == %u) { oC[0] = tex2_%u.SampleLevel(smp%u, pin.i[4].xy, 0); oC[0].w = 1.0; }\n    ", 300 + s, s, s, 400 + s, s, s);
            size_t pp = src.find("TEXDBG");
            if (pp != std::string::npos) src.replace(pp, 6, dbg);
        }
        auto rep = [&](const char* k, const char* v) { size_t p = src.find(k); if (p != std::string::npos) src.replace(p, strlen(k), v); };
        rep("DEPTHMEMBER", out.writesDepth ? "float depth : SV_Depth; " : "");
        rep("DEPTHASSIGN", out.writesDepth ? "o.depth = oDepth;" : "");
    }
    out.hlsl = src;
    out.numRegs = t.maxReg;
    out.ok = true;
    return true;
}
