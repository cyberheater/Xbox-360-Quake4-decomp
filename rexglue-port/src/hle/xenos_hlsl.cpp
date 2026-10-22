// q4hle - Xenos shader microcode to HLSL (Shader Model 5) translator. See xenos_hlsl.h.

#include "hle/xenos_hlsl.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <rex/graphics/format/ucode.h>

namespace q4hle::xenos {
namespace {

namespace uc = rex::graphics::ucode;
namespace xe = rex::graphics::xenos;

std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

const char kComp[] = "xyzw";

const char kCommon[] = R"(
// ---- q4hle Xenos common ----
cbuffer XeDraw : register(b1) {
  uint4 xe_bool[2];
  uint4 xe_loop[32];
  float4 xe_ndc_scale;
  float4 xe_ndc_offset;
  float4 xe_alpha;     // x ref, y func, z enabled
  uint4 xe_tex[32];    // x swizzle, y signs, z exp-adjust scale (float bits), w flags
  float4 xe_misc;      // x: half pixel offset enabled, y: alpha-to-mask offsets, z: 1/resolution scale
  float4 xe_point;     // point sprites: x,y constant diameter, z min, w max (guest pixels)
  float4 xe_point2;    // x,y: diameter -> NDC radius
};
bool XeBool(uint i) { return (xe_bool[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1; }
// Direct3D 9 / Xenos multiplication: 0 * anything = 0 (including inf and NaN).
float  xmul(float a, float b)   { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 xmul4(float4 a, float4 b) { return float4(xmul(a.x, b.x), xmul(a.y, b.y), xmul(a.z, b.z), xmul(a.w, b.w)); }
float  xdot4(float4 a, float4 b) { return xmul(a.x, b.x) + xmul(a.y, b.y) + xmul(a.z, b.z) + xmul(a.w, b.w); }
float  xdot3(float4 a, float4 b) { return xmul(a.x, b.x) + xmul(a.y, b.y) + xmul(a.z, b.z); }
float  xmax(float a, float b) { return a >= b ? a : b; }
float  xmin(float a, float b) { return a < b ? a : b; }
float4 xmax4(float4 a, float4 b) { return float4(xmax(a.x,b.x), xmax(a.y,b.y), xmax(a.z,b.z), xmax(a.w,b.w)); }
float4 xmin4(float4 a, float4 b) { return float4(xmin(a.x,b.x), xmin(a.y,b.y), xmin(a.z,b.z), xmin(a.w,b.w)); }
static const float XE_FLT_MAX = 3.402823466e+38;
float xclampinf(float v) { return isinf(v) ? (v < 0.0 ? -XE_FLT_MAX : XE_FLT_MAX) : v; }
float xflushinf(float v) { return isinf(v) ? 0.0 : v; }
float xlogc(float v) { float r = log2(v); return isinf(r) ? -XE_FLT_MAX : r; }
// Texture fetch-constant swizzle (3 bits per component: 0-3 = xyzw, 4 = 0, 5 = 1).
float XeSwzComp(float4 v, uint s) { return s < 4 ? v[s] : (s == 5 ? 1.0 : 0.0); }
float4 XeTexPost(float4 v, uint slot) {
  uint4 info = xe_tex[slot];
  // signs: 2 bits per component: 0 unsigned, 1 signed, 2 unsigned biased (2x-1), 3 gamma
  float4 r;
  [unroll] for (uint i = 0; i < 4; ++i) {
    uint sg = (info.y >> (2 * i)) & 3;
    float c = v[i];
    if (sg == 2) c = c * 2.0 - 1.0;
    else if (sg == 3) c = pow(saturate(c), 2.2);
    r[i] = c;
  }
  r *= asfloat(info.z);
  return float4(XeSwzComp(r, info.x & 7), XeSwzComp(r, (info.x >> 3) & 7),
                XeSwzComp(r, (info.x >> 6) & 7), XeSwzComp(r, (info.x >> 9) & 7));
}
float4 XeCube(float3 d) {
  float3 a = abs(d);
  if (a.z >= a.x && a.z >= a.y) return float4(-d.y, d.z < 0.0 ? -d.x : d.x, 2.0 * d.z, d.z < 0.0 ? 5.0 : 4.0);
  if (a.y >= a.x) return float4(d.y < 0.0 ? -d.z : d.z, d.x, 2.0 * d.y, d.y < 0.0 ? 3.0 : 2.0);
  return float4(-d.y, d.x < 0.0 ? d.z : -d.z, 2.0 * d.x, d.x < 0.0 ? 1.0 : 0.0);
}
float3 XeCubeDir(float3 c) {
  float s = (c.x - 1.5) * 2.0, t = (c.y - 1.5) * 2.0;
  uint f = (uint)(c.z + 0.5);
  if (f == 0) return float3(1.0, -t, -s);
  if (f == 1) return float3(-1.0, -t, s);
  if (f == 2) return float3(s, 1.0, t);
  if (f == 3) return float3(s, -1.0, -t);
  if (f == 4) return float3(s, -t, 1.0);
  return float3(-s, -t, -1.0);
}
// Vertex fetch helpers (buffers are little-endian dwords; the renderer swaps on upload).
float XeUnorm(uint v, uint bits) { return (float)v / (float)((1u << bits) - 1u); }
float XeSnorm(uint v, uint bits) {
  int s = (int)(v << (32 - bits)) >> (32 - bits);
  return max((float)s / (float)((1u << (bits - 1)) - 1u), -1.0);
}
float XeSint(uint v, uint bits) { return (float)((int)(v << (32 - bits)) >> (32 - bits)); }
)";

struct Gen {
  const uint32_t* ucode;
  size_t dwords;
  bool pixel;
  TranslateOptions opt;
  TranslatedShader out;
  std::string body;
  std::string dis;
  int indent = 1;
  // last full vertex fetch (mini fetches reuse its index and fetch constant)
  std::string vf_index_expr = "0";
  uint32_t vf_const = 0;
  uint32_t vf_stride = 0;
  bool uses_a0 = false, uses_lod = false;
  bool flow_loop = false;  // control flow is emitted as a loop: no implicit derivatives
  uint32_t sampler_count = 0;

  void Line(const std::string& s) {
    body.append(indent * 2, ' ');
    body += s;
    body += '\n';
  }
  void Err(const std::string& s) {
    if (out.error.find(s) == std::string::npos) out.error += s + "; ";
  }
  void UseTemp(uint32_t r) {
    if (r + 1 > out.max_temp) out.max_temp = r + 1;
  }

  // ---- ALU operands ----
  std::string SrcReg(const uc::AluInstruction& in, int i) {
    const uint32_t reg = in.src_reg(i);
    if (in.src_is_temp(i)) {
      const uint32_t r = uc::AluInstruction::src_temp_reg(reg);
      UseTemp(r);
      std::string s = uc::AluInstruction::is_src_temp_relative(reg) ? Fmt("r[aL + %u]", r) : Fmt("r[%u]", r);
      if (uc::AluInstruction::is_src_temp_value_absolute(reg)) s = "abs(" + s + ")";
      return s;
    }
    std::string s;
    if (in.src_const_is_addressed(i)) {
      if (in.is_const_address_register_relative()) {
        uses_a0 = true;
        s = Fmt("c[(a0 + %u) & 255]", reg);
      } else {
        s = Fmt("c[(aL + %u) & 255]", reg);
      }
    } else {
      s = Fmt("c[%u]", reg);
    }
    if (in.abs_constants()) s = "abs(" + s + ")";
    return s;
  }
  std::string Swz(uint32_t swz) {
    std::string s = ".";
    for (uint32_t c = 0; c < 4; ++c) s += kComp[uc::AluInstruction::GetSwizzledComponentIndex(swz, c)];
    return s;
  }
  std::string VecSrc(const uc::AluInstruction& in, int i) {
    std::string s = SrcReg(in, i) + Swz(in.src_swizzle(i));
    if (in.src_negate(i)) s = "(-" + s + ")";
    return s;
  }
  // scalar operands a (W) and b (X) from operand 3
  void ScalarAB(const uc::AluInstruction& in, std::string& a, std::string& b) {
    const uint32_t swz = in.src_swizzle(3);
    const uint32_t ca = ((swz >> 6) + 3) & 3, cb = swz & 3;
    std::string base = SrcReg(in, 3);
    a = base + "." + kComp[ca];
    b = base + "." + kComp[cb];
    if (in.src_negate(3)) {
      a = "(-" + a + ")";
      b = "(-" + b + ")";
    }
  }

  std::string VectorOp(const uc::AluInstruction& in, bool& kill, bool& sets_pred, std::string& pred_expr) {
    using V = uc::AluVectorOpcode;
    const V op = in.vector_opcode();
    auto A = [&] { return VecSrc(in, 1); };
    auto B = [&] { return VecSrc(in, 2); };
    auto C = [&] { return VecSrc(in, 3); };
    switch (op) {
      case V::kAdd: return A() + " + " + B();
      case V::kMul: return "xmul4(" + A() + ", " + B() + ")";
      case V::kMax: return "xmax4(" + A() + ", " + B() + ")";
      case V::kMin: return "xmin4(" + A() + ", " + B() + ")";
      case V::kSeq: return "float4(" + A() + " == " + B() + ")";
      case V::kSgt: return "float4(" + A() + " > " + B() + ")";
      case V::kSge: return "float4(" + A() + " >= " + B() + ")";
      case V::kSne: return "float4(" + A() + " != " + B() + ")";
      case V::kFrc: return "frac(" + A() + ")";
      case V::kTrunc: return "trunc(" + A() + ")";
      case V::kFloor: return "floor(" + A() + ")";
      case V::kMad: return "xmul4(" + A() + ", " + B() + ") + " + C();
      case V::kCndEq: return "((" + A() + " == 0.0) ? " + B() + " : " + C() + ")";
      case V::kCndGe: return "((" + A() + " >= 0.0) ? " + B() + " : " + C() + ")";
      case V::kCndGt: return "((" + A() + " > 0.0) ? " + B() + " : " + C() + ")";
      case V::kDp4: return "xdot4(" + A() + ", " + B() + ").xxxx";
      case V::kDp3: return "xdot3(" + A() + ", " + B() + ").xxxx";
      case V::kDp2Add: {
        const std::string a = A(), b = B();
        return "(xmul(" + a + ".x, " + b + ".x) + xmul(" + a + ".y, " + b + ".y) + " + C() + ".x).xxxx";
      }
      case V::kCube: {
        // The operand is .z_xy: direction = (src.z, src.w, src.x). Result: tc, sc, 2 * ma, face.
        const std::string a = A();
        return "XeCube(float3(" + a + ".z, " + a + ".w, " + a + ".x))";
      }
      case V::kMax4: {
        const std::string a = A();
        return "xmax(xmax(" + a + ".x, " + a + ".y), xmax(" + a + ".z, " + a + ".w)).xxxx";
      }
      case V::kSetpEqPush:
      case V::kSetpNePush:
      case V::kSetpGtPush:
      case V::kSetpGePush: {
        const char* cmp = op == V::kSetpEqPush ? "== 0.0" : op == V::kSetpNePush ? "!= 0.0" : op == V::kSetpGtPush ? "> 0.0" : ">= 0.0";
        const std::string a = A(), b = B();
        sets_pred = true;
        pred_expr = "(" + a + ".w == 0.0 && " + b + ".w " + cmp + ")";
        return "(((" + a + ".x == 0.0 && " + b + ".x " + cmp + ") ? 0.0 : " + a + ".x + 1.0).xxxx)";
      }
      case V::kKillEq:
      case V::kKillGt:
      case V::kKillGe:
      case V::kKillNe: {
        const char* cmp = op == V::kKillEq ? "==" : op == V::kKillGt ? ">" : op == V::kKillGe ? ">=" : "!=";
        kill = true;
        return "(any(" + A() + " " + cmp + " " + B() + ") ? 1.0 : 0.0).xxxx";
      }
      case V::kDst: {
        const std::string a = A(), b = B();
        return "float4(1.0, xmul(" + a + ".y, " + b + ".y), " + a + ".z, " + b + ".w)";
      }
      case V::kMaxA: {
        const std::string a = A(), b = B();
        uses_a0 = true;
        Line("a0 = clamp((int)floor(" + a + ".w + 0.5), -256, 255);");
        return "xmax4(" + a + ", " + b + ")";
      }
      default:
        Err(Fmt("vector opcode %u", uint32_t(op)));
        return "float4(0.0, 0.0, 0.0, 0.0)";
    }
  }

  // Returns the scalar expression; may emit statements (predicate updates).
  std::string ScalarOp(const uc::AluInstruction& in, bool& kill, bool& pred_written, std::string& pred_expr) {
    using S = uc::AluScalarOpcode;
    const S op = in.scalar_opcode();
    std::string a, b;
    ScalarAB(in, a, b);
    switch (op) {
      case S::kAdds: return a + " + " + b;
      case S::kAddsPrev: return a + " + ps";
      case S::kMuls: return "xmul(" + a + ", " + b + ")";
      case S::kMulsPrev: return "xmul(" + a + ", ps)";
      case S::kMulsPrev2:
        return "((ps == -XE_FLT_MAX || isinf(ps) || isnan(ps) || isinf(" + b + ") || isnan(" + b + ") || " + b +
               " <= 0.0) ? -XE_FLT_MAX : xmul(" + a + ", ps))";
      case S::kMaxs: return "xmax(" + a + ", " + b + ")";
      case S::kMins: return "xmin(" + a + ", " + b + ")";
      case S::kSeqs: return "float(" + a + " == 0.0)";
      case S::kSgts: return "float(" + a + " > 0.0)";
      case S::kSges: return "float(" + a + " >= 0.0)";
      case S::kSnes: return "float(" + a + " != 0.0)";
      case S::kFrcs: return "frac(" + a + ")";
      case S::kTruncs: return "trunc(" + a + ")";
      case S::kFloors: return "floor(" + a + ")";
      case S::kExp: return "exp2(" + a + ")";
      case S::kLogc: return "xlogc(" + a + ")";
      case S::kLog: return "log2(" + a + ")";
      case S::kRcpc: return "xclampinf(1.0 / " + a + ")";
      case S::kRcpf: return "xflushinf(1.0 / " + a + ")";
      case S::kRcp: return "(1.0 / " + a + ")";
      case S::kRsqc: return "xclampinf(rsqrt(" + a + "))";
      case S::kRsqf: return "xflushinf(rsqrt(" + a + "))";
      case S::kRsq: return "rsqrt(" + a + ")";
      case S::kMaxAs:
        uses_a0 = true;
        Line("a0 = clamp((int)floor(" + a + " + 0.5), -256, 255);");
        return "xmax(" + a + ", " + b + ")";
      case S::kMaxAsf:
        uses_a0 = true;
        Line("a0 = clamp((int)floor(" + a + "), -256, 255);");
        return "xmax(" + a + ", " + b + ")";
      case S::kSubs: return a + " - " + b;
      case S::kSubsPrev: return a + " - ps";
      case S::kSetpEq: pred_written = true; pred_expr = "(" + a + " == 0.0)"; return "(" + a + " == 0.0 ? 0.0 : 1.0)";
      case S::kSetpNe: pred_written = true; pred_expr = "(" + a + " != 0.0)"; return "(" + a + " != 0.0 ? 0.0 : 1.0)";
      case S::kSetpGt: pred_written = true; pred_expr = "(" + a + " > 0.0)"; return "(" + a + " > 0.0 ? 0.0 : 1.0)";
      case S::kSetpGe: pred_written = true; pred_expr = "(" + a + " >= 0.0)"; return "(" + a + " >= 0.0 ? 0.0 : 1.0)";
      case S::kSetpInv:
        pred_written = true;
        pred_expr = "(" + a + " == 1.0)";
        return "(" + a + " == 1.0 ? 0.0 : (" + a + " == 0.0 ? 1.0 : " + a + "))";
      case S::kSetpPop:
        pred_written = true;
        pred_expr = "(" + a + " - 1.0 <= 0.0)";
        return "(" + a + " - 1.0 <= 0.0 ? 0.0 : " + a + " - 1.0)";
      case S::kSetpClr: pred_written = true; pred_expr = "false"; return "XE_FLT_MAX";
      case S::kSetpRstr: pred_written = true; pred_expr = "(" + a + " == 0.0)"; return a;
      case S::kKillsEq: kill = true; return "float(" + a + " == 0.0)";
      case S::kKillsGt: kill = true; return "float(" + a + " > 0.0)";
      case S::kKillsGe: kill = true; return "float(" + a + " >= 0.0)";
      case S::kKillsNe: kill = true; return "float(" + a + " != 0.0)";
      case S::kKillsOne: kill = true; return "float(" + a + " == 1.0)";
      case S::kSqrt: return "sqrt(" + a + ")";
      case S::kMulsc0:
      case S::kMulsc1:
      case S::kAddsc0:
      case S::kAddsc1:
      case S::kSubsc0:
      case S::kSubsc1: {
        // constant operand (a component of operand 3 as a constant) and a temporary (b)
        const uint32_t swz = in.src_swizzle(3);
        const uint32_t ca = ((swz >> 6) + 3) & 3, cb = swz & 3;
        const uint32_t treg = in.scalar_const_reg_op_src_temp_reg();
        UseTemp(treg);
        std::string cst;
        const uint32_t creg = in.src_reg(3);
        // the constant uses the addressing of operand 3 regardless of src3_sel
        if (in.src_const_is_addressed(3))
          cst = in.is_const_address_register_relative() ? Fmt("c[(a0 + %u) & 255]", creg) : Fmt("c[(aL + %u) & 255]", creg);
        else
          cst = Fmt("c[%u]", creg);
        if (in.is_const_address_register_relative() && in.src_const_is_addressed(3)) uses_a0 = true;
        if (in.abs_constants()) cst = "abs(" + cst + ")";
        std::string ea = cst + "." + kComp[ca];
        std::string eb = Fmt("r[%u].%c", treg, kComp[cb]);
        if (in.src_negate(3)) {
          ea = "(-" + ea + ")";
          eb = "(-" + eb + ")";
        }
        if (op == S::kMulsc0 || op == S::kMulsc1) return "xmul(" + ea + ", " + eb + ")";
        if (op == S::kAddsc0 || op == S::kAddsc1) return ea + " + " + eb;
        return ea + " - " + eb;
      }
      case S::kSin: return "sin(" + a + ")";
      case S::kCos: return "cos(" + a + ")";
      case S::kRetainPrev: return "ps";
      default:
        Err(Fmt("scalar opcode %u", uint32_t(op)));
        return "0.0";
    }
  }

  static bool VectorOpUsesSrc(uc::AluVectorOpcode op, int i) {
    using V = uc::AluVectorOpcode;
    switch (op) {
      case V::kFrc: case V::kTrunc: case V::kFloor: case V::kMax4: case V::kCube:
        return i == 1;
      case V::kMad: case V::kCndEq: case V::kCndGe: case V::kCndGt: case V::kDp2Add:
        return true;
      default:
        return i <= 2;
    }
  }

  std::string ExportTarget(uint32_t reg, bool& ok) {
    ok = true;
    if (pixel) {
      if (reg <= 3) {
        out.color_targets |= 1u << reg;
        return Fmt("oC%u", reg);
      }
      if (reg == 61) {
        out.writes_depth = true;
        return "oDepth4";
      }
    } else {
      if (reg <= 15) return Fmt("o[%u]", reg);
      if (reg == 62) return "oPos";
      if (reg == 63) return "oPts";
    }
    ok = false;
    Err(Fmt("export register %u", reg));
    return "dummy";
  }

  void EmitAlu(const uc::AluInstruction& in) {
    using V = uc::AluVectorOpcode;
    using S = uc::AluScalarOpcode;
    const bool vec_write = in.vector_write_mask() != 0 || in.vector_opcode() == V::kMaxA ||
                           (in.vector_opcode() >= V::kSetpEqPush && in.vector_opcode() <= V::kKillNe);
    const bool scalar_active = in.scalar_opcode() != S::kRetainPrev || in.scalar_write_mask() != 0;
    if (in.is_predicated()) {
      Line(std::string("if (") + (in.predicate_condition() ? "" : "!") + "p0) {");
      ++indent;
    }
    Line("{");
    ++indent;
    bool vkill = false, skill = false, vpred = false, spred = false;
    std::string vpred_expr, spred_expr;
    std::string vexpr, sexpr;
    // Evaluate both operations from the unmodified registers first.
    if (vec_write || in.is_export()) {
      vexpr = VectorOp(in, vkill, vpred, vpred_expr);
      Line("float4 vr = " + vexpr + ";");
      if (in.vector_clamp()) Line("vr = saturate(vr);");
    }
    sexpr = ScalarOp(in, skill, spred, spred_expr);
    Line("float sr = " + sexpr + ";");
    if (in.scalar_clamp()) Line("sr = saturate(sr);");
    if (vpred) Line("p0 = " + vpred_expr + ";");
    if (spred) Line("p0 = " + spred_expr + ";");
    if (vkill || skill) {
      out.uses_kill = true;
      if (pixel) {
        if (vkill) Line("if (vr.x != 0.0) discard;");
        if (skill) Line("if (sr != 0.0) discard;");
      }
    }
    const uint32_t vmask = in.vector_write_mask(), smask = in.scalar_write_mask();
    if (in.is_export()) {
      bool ok;
      const std::string t = ExportTarget(in.vector_dest(), ok);
      if (ok) {
        for (uint32_t c = 0; c < 4; ++c) {
          const bool v = vmask & (1u << c), s = smask & (1u << c);
          if (v && s) Line(t + "." + kComp[c] + " = 1.0;");
          else if (v) Line(t + "." + kComp[c] + " = vr." + kComp[c] + ";");
          else if (s) Line(t + "." + kComp[c] + " = sr;");
          else if (in.is_scalar_dest_relative()) Line(t + "." + kComp[c] + " = 0.0;");
        }
      }
    } else {
      if (vmask) {
        UseTemp(in.vector_dest());
        std::string d = in.is_vector_dest_relative() ? Fmt("r[aL + %u]", in.vector_dest()) : Fmt("r[%u]", in.vector_dest());
        std::string m;
        for (uint32_t c = 0; c < 4; ++c)
          if (vmask & (1u << c)) m += kComp[c];
        Line(d + "." + m + " = vr." + m + ";");
      }
      if (smask) {
        UseTemp(in.scalar_dest());
        std::string d = in.is_scalar_dest_relative() ? Fmt("r[aL + %u]", in.scalar_dest()) : Fmt("r[%u]", in.scalar_dest());
        std::string m;
        for (uint32_t c = 0; c < 4; ++c)
          if (smask & (1u << c)) m += kComp[c];
        Line(d + "." + m + " = sr;");
      }
    }
    if (scalar_active) Line("ps = sr;");
    --indent;
    Line("}");
    if (in.is_predicated()) {
      --indent;
      Line("}");
    }
  }

  // ---- fetch ----
  void EmitDestSwizzle(const std::string& value, uint32_t dest, bool rel, uint32_t swz) {
    UseTemp(dest);
    const std::string d = rel ? Fmt("r[aL + %u]", dest) : Fmt("r[%u]", dest);
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t s = (swz >> (3 * c)) & 7;
      if (s == 7) continue;
      std::string v = s < 4 ? value + "." + kComp[s] : (s == 5 ? "1.0" : "0.0");
      Line(d + "." + kComp[c] + " = " + v + ";");
    }
  }

  void EmitVertexFetch(const uc::VertexFetchInstruction& vf) {
    if (pixel) {
      Err("vertex fetch in pixel shader");
      return;
    }
    if (vf.is_predicated()) {
      Line(std::string("if (") + (vf.predicate_condition() ? "" : "!") + "p0) {");
      ++indent;
    }
    if (!vf.is_mini_fetch()) {
      UseTemp(vf.src());
      const std::string src = (vf.is_src_relative() ? Fmt("r[aL + %u]", vf.src()) : Fmt("r[%u]", vf.src())) + "." +
                              kComp[vf.src_swizzle() & 3];
      vf_index_expr = vf.is_index_rounded() ? "(uint)floor(" + src + " + 0.5)" : "(uint)floor(" + src + ")";
      vf_const = vf.fetch_constant_index();
      vf_stride = vf.stride();
      Line(Fmt("vf_index = %s;", vf_index_expr.c_str()));
    }
    const uint32_t fc = vf_const;
    out.vfetch_mask[fc >> 5] |= 1u << (fc & 31);
    if (!out.vf_stride_dw[fc]) out.vf_stride_dw[fc] = static_cast<uint16_t>(vf_stride);
    {
      uint32_t elem = 1;
      switch (vf.data_format()) {
        case xe::VertexFormat::k_32_32: case xe::VertexFormat::k_32_32_FLOAT: case xe::VertexFormat::k_16_16_16_16:
        case xe::VertexFormat::k_16_16_16_16_FLOAT: elem = 2; break;
        case xe::VertexFormat::k_32_32_32_FLOAT: elem = 3; break;
        case xe::VertexFormat::k_32_32_32_32: case xe::VertexFormat::k_32_32_32_32_FLOAT: elem = 4; break;
        default: elem = 1; break;
      }
      const int32_t ext = vf.offset() + int32_t(elem);
      if (ext > 0 && uint32_t(ext) > out.vf_extent_dw[fc]) out.vf_extent_dw[fc] = uint32_t(ext);
    }
    const std::string buf = Fmt("vb%u", fc);
    const std::string addr = Fmt("(vf_index * %uu + %du) * 4u", vf_stride, vf.offset());
    std::string val;
    const bool sgn = vf.is_signed(), norm = vf.is_normalized();
    auto int_comp = [&](const std::string& bits_expr, uint32_t bits) -> std::string {
      if (norm) return sgn ? Fmt("XeSnorm(%s, %uu)", bits_expr.c_str(), bits) : Fmt("XeUnorm(%s, %uu)", bits_expr.c_str(), bits);
      return sgn ? Fmt("XeSint(%s, %uu)", bits_expr.c_str(), bits) : Fmt("(float)(%s)", bits_expr.c_str());
    };
    using F = xe::VertexFormat;
    Line("{");
    ++indent;
    switch (vf.data_format()) {
      case F::k_32_FLOAT:
        Line(Fmt("float4 f = float4(asfloat(%s.Load(%s)), 0.0, 0.0, 1.0);", buf.c_str(), addr.c_str()));
        break;
      case F::k_32_32_FLOAT:
        Line(Fmt("float4 f = float4(asfloat(%s.Load2(%s)), 0.0, 1.0);", buf.c_str(), addr.c_str()));
        break;
      case F::k_32_32_32_FLOAT:
        Line(Fmt("float4 f = float4(asfloat(%s.Load3(%s)), 1.0);", buf.c_str(), addr.c_str()));
        break;
      case F::k_32_32_32_32_FLOAT:
        Line(Fmt("float4 f = asfloat(%s.Load4(%s));", buf.c_str(), addr.c_str()));
        break;
      case F::k_32:
      case F::k_32_32:
      case F::k_32_32_32_32: {
        const int n = vf.data_format() == F::k_32 ? 1 : vf.data_format() == F::k_32_32 ? 2 : 4;
        Line(Fmt("uint4 u = %s.Load4(%s);", buf.c_str(), addr.c_str()));
        std::string comps[4] = {"0.0", "0.0", "0.0", "1.0"};
        for (int i = 0; i < n; ++i) {
          const std::string e = std::string("u.") + kComp[i];
          comps[i] = norm ? (sgn ? "max((float)(int)" + e + " / 2147483647.0, -1.0)" : "(float)" + e + " / 4294967295.0")
                          : (sgn ? "(float)(int)" + e : "(float)" + e);
        }
        Line("float4 f = float4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ");");
        break;
      }
      case F::k_8_8_8_8:
        Line(Fmt("uint u = %s.Load(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(" + int_comp("(u & 0xFFu)", 8) + ", " + int_comp("((u >> 8) & 0xFFu)", 8) + ", " +
             int_comp("((u >> 16) & 0xFFu)", 8) + ", " + int_comp("(u >> 24)", 8) + ");");
        break;
      case F::k_2_10_10_10:
        Line(Fmt("uint u = %s.Load(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(" + int_comp("(u & 0x3FFu)", 10) + ", " + int_comp("((u >> 10) & 0x3FFu)", 10) + ", " +
             int_comp("((u >> 20) & 0x3FFu)", 10) + ", " + int_comp("(u >> 30)", 2) + ");");
        break;
      case F::k_10_11_11:
        Line(Fmt("uint u = %s.Load(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(" + int_comp("(u & 0x7FFu)", 11) + ", " + int_comp("((u >> 11) & 0x7FFu)", 11) + ", " +
             int_comp("(u >> 22)", 10) + ", 1.0);");
        break;
      case F::k_11_11_10:
        Line(Fmt("uint u = %s.Load(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(" + int_comp("(u & 0x3FFu)", 10) + ", " + int_comp("((u >> 10) & 0x7FFu)", 11) + ", " +
             int_comp("(u >> 21)", 11) + ", 1.0);");
        break;
      case F::k_16_16:
        Line(Fmt("uint u = %s.Load(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(" + int_comp("(u & 0xFFFFu)", 16) + ", " + int_comp("(u >> 16)", 16) + ", 0.0, 1.0);");
        break;
      case F::k_16_16_16_16:
        Line(Fmt("uint2 u = %s.Load2(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(" + int_comp("(u.x & 0xFFFFu)", 16) + ", " + int_comp("(u.x >> 16)", 16) + ", " +
             int_comp("(u.y & 0xFFFFu)", 16) + ", " + int_comp("(u.y >> 16)", 16) + ");");
        break;
      case F::k_16_16_FLOAT:
        Line(Fmt("uint u = %s.Load(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(f16tof32(u & 0xFFFFu), f16tof32(u >> 16), 0.0, 1.0);");
        break;
      case F::k_16_16_16_16_FLOAT:
        Line(Fmt("uint2 u = %s.Load2(%s);", buf.c_str(), addr.c_str()));
        Line("float4 f = float4(f16tof32(u.x & 0xFFFFu), f16tof32(u.x >> 16), f16tof32(u.y & 0xFFFFu), f16tof32(u.y >> 16));");
        break;
      default:
        Err(Fmt("vertex format %u", uint32_t(vf.data_format())));
        Line("float4 f = float4(0.0, 0.0, 0.0, 1.0);");
        break;
    }
    if (vf.exp_adjust() != 0) Line(Fmt("f *= %.9g;", std::ldexp(1.0, vf.exp_adjust())));
    EmitDestSwizzle("f", vf.dest(), vf.is_dest_relative(), vf.dest_swizzle());
    --indent;
    Line("}");
    if (vf.is_predicated()) {
      --indent;
      Line("}");
    }
  }

  void EmitTextureFetch(const uc::TextureFetchInstruction& tf) {
    using O = uc::FetchOpcode;
    const uint32_t slot = tf.fetch_constant_index();
    if (tf.is_predicated()) {
      Line(std::string("if (") + (tf.predicate_condition() ? "" : "!") + "p0) {");
      ++indent;
    }
    UseTemp(tf.src());
    const std::string src = tf.is_src_relative() ? Fmt("r[aL + %u]", tf.src()) : Fmt("r[%u]", tf.src());
    std::string coord = src + ".";
    for (uint32_t c = 0; c < 3; ++c) coord += kComp[(tf.src_swizzle() >> (2 * c)) & 3];
    TexDim dim = TexDim::k2D;
    switch (tf.dimension()) {
      case xe::FetchOpDimension::k1D: dim = TexDim::k1D; break;
      case xe::FetchOpDimension::k2D: dim = TexDim::k2D; break;
      case xe::FetchOpDimension::k3DOrStacked: dim = TexDim::k3D; break;
      case xe::FetchOpDimension::kCube: dim = TexDim::kCube; break;
    }
    if (tf.opcode() == O::kSetTextureLod) {
      uses_lod = true;
      Line("tex_lod = " + coord + ".x;");
    } else if (tf.opcode() == O::kSetTextureGradientsHorz || tf.opcode() == O::kSetTextureGradientsVert) {
      // explicit gradients: not supported, implicit derivatives are used
    } else if (tf.opcode() == O::kTextureFetch || tf.opcode() == O::kGetTextureComputedLod ||
               tf.opcode() == O::kGetTextureWeights || tf.opcode() == O::kGetTextureBorderColorFrac ||
               tf.opcode() == O::kGetTextureGradients) {
      if (out.tex_dim[slot] != TexDim::kNone && out.tex_dim[slot] != dim) Err(Fmt("texture slot %u used with two dimensions", slot));
      out.tex_dim[slot] = dim;
      if (out.sampler_slot[slot] < 0) out.sampler_slot[slot] = static_cast<int8_t>(sampler_count++ & 15);
      const std::string tex = Fmt("tex%u", slot);
      const std::string smp = Fmt("smp%d", out.sampler_slot[slot]);
      Line("{");
      ++indent;
      // coordinates
      std::string cexpr;
      switch (dim) {
        case TexDim::k1D: cexpr = coord + ".x"; break;
        case TexDim::k2D: cexpr = coord + ".xy"; break;
        case TexDim::k3D: cexpr = coord + ".xyz"; break;
        case TexDim::kCube: cexpr = "XeCubeDir(" + coord + ".xyz)"; break;
        default: break;
      }
      const char* ctype = dim == TexDim::k1D ? "float" : dim == TexDim::k2D ? "float2" : "float3";
      Line(Fmt("%s tc = %s;", ctype, cexpr.c_str()));
      if (dim != TexDim::kCube && (tf.unnormalized_coordinates() || tf.offset_x() != 0 || tf.offset_y() != 0 || tf.offset_z() != 0)) {
        switch (dim) {
          case TexDim::k1D: Line("float tsz; " + tex + ".GetDimensions(tsz);"); break;
          case TexDim::k2D: Line("float2 tsz; " + tex + ".GetDimensions(tsz.x, tsz.y);"); break;
          default: Line("float3 tsz; " + tex + ".GetDimensions(tsz.x, tsz.y, tsz.z);"); break;
        }
        const std::string off = dim == TexDim::k1D ? Fmt("%.4g", tf.offset_x())
                                : dim == TexDim::k2D ? Fmt("float2(%.4g, %.4g)", tf.offset_x(), tf.offset_y())
                                                     : Fmt("float3(%.4g, %.4g, %.4g)", tf.offset_x(), tf.offset_y(), tf.offset_z());
        if (tf.unnormalized_coordinates()) Line("tc = (tc + " + off + ") / tsz;");
        else Line("tc += " + off + " / tsz;");
      }
      if (tf.opcode() == O::kTextureFetch) {
        std::string sample;
        const float bias = tf.lod_bias();
        if (!pixel || flow_loop) {
          sample = tex + ".SampleLevel(" + smp + ", tc, 0.0)";
        } else if (tf.use_register_lod() && !tf.use_computed_lod()) {
          uses_lod = true;
          sample = tex + Fmt(".SampleLevel(%s, tc, tex_lod + %.4g)", smp.c_str(), bias);
        } else if (tf.use_register_lod()) {
          uses_lod = true;
          sample = tex + Fmt(".SampleBias(%s, tc, tex_lod + %.4g)", smp.c_str(), bias);
        } else if (bias != 0.0f) {
          sample = tex + Fmt(".SampleBias(%s, tc, %.4g)", smp.c_str(), bias);
        } else {
          sample = tex + ".Sample(" + smp + ", tc)";
        }
        Line("float4 t = XeTexPost(" + sample + Fmt(", %uu);", slot));
      } else if (tf.opcode() == O::kGetTextureComputedLod) {
        if (pixel && dim != TexDim::k3D)
          Line("float4 t = " + tex + ".CalculateLevelOfDetailUnclamped(" + smp + ", tc).xxxx;");
        else
          Line("float4 t = 0.0;");
      } else if (tf.opcode() == O::kGetTextureWeights) {
        if (dim == TexDim::k2D) {
          Line("float2 tsz2; " + tex + ".GetDimensions(tsz2.x, tsz2.y);");
          Line("float4 t = float4(frac(tc * tsz2 - 0.5), 0.0, 0.0);");
        } else {
          Line("float4 t = 0.0;");
        }
      } else {
        Line("float4 t = 0.0;");
      }
      EmitDestSwizzle("t", tf.dest(), tf.is_dest_relative(), tf.dest_swizzle());
      --indent;
      Line("}");
    } else {
      Err(Fmt("fetch opcode %u", uint32_t(tf.opcode())));
    }
    if (tf.is_predicated()) {
      --indent;
      Line("}");
    }
  }

  void EmitExec(uint32_t address, uint32_t count, uint32_t sequence) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t idx = address + i;
      if (idx * 3 + 2 >= dwords) {
        Err("instruction out of range");
        return;
      }
      const uint32_t* w = ucode + idx * 3;
      const bool is_fetch = (sequence >> (i * 2)) & 1;
      dis += Fmt("    %3u %s %08X %08X %08X\n", idx, is_fetch ? "fetch" : "alu  ", w[0], w[1], w[2]);
      Line(Fmt("// %u: %s %08X %08X %08X", idx, is_fetch ? "fetch" : "alu", w[0], w[1], w[2]));
      if (is_fetch) {
        uc::FetchInstruction f;
        std::memcpy(&f, w, 12);
        if (f.opcode() == uc::FetchOpcode::kVertexFetch)
          EmitVertexFetch(f.vertex_fetch());
        else
          EmitTextureFetch(f.texture_fetch());
      } else {
        uc::AluInstruction a;
        std::memcpy(&a, w, 12);
        EmitAlu(a);
      }
    }
  }

  bool Run() {
    using O = uc::ControlFlowOpcode;
    // Control flow: pairs of 48-bit instructions in 3-dword groups. Parse until an end-of-shader
    // instruction that lies beyond every jump target.
    std::vector<uc::ControlFlowInstruction> cf;
    uint32_t max_target = 0;
    for (size_t i = 0; i + 2 < dwords; i += 3) {
      uc::ControlFlowInstruction ab[2];
      uc::UnpackControlFlowInstructions(ucode + i, ab);
      bool end = false;
      for (auto& c : ab) {
        const uint32_t idx = uint32_t(cf.size());
        cf.push_back(c);
        switch (c.opcode()) {
          case O::kCondJmp: max_target = std::max(max_target, c.cond_jmp.address()); break;
          case O::kLoopStart: max_target = std::max(max_target, c.loop_start.address()); break;
          case O::kCondCall: max_target = std::max(max_target, c.cond_call.address()); break;
          default: break;
        }
        if (uc::DoesControlFlowOpcodeEndShader(c.opcode()) && idx >= max_target) end = true;
      }
      if (end) break;
      if (cf.size() > 512) break;
    }
    // Control flow graph over the CF instructions: only what is reachable from 0 is emitted
    // (the parse above can run into padding or data after the real end of the shader).
    const size_t n = cf.size();
    auto succ = [&](size_t i, uint32_t out_s[2]) -> int {
      const auto& c = cf[i];
      const O op = c.opcode();
      if (uc::DoesControlFlowOpcodeEndShader(op)) return 0;
      int k = 0;
      switch (op) {
        case O::kCondJmp:
          if (c.cond_jmp.address() < n) out_s[k++] = c.cond_jmp.address();
          if (!c.cond_jmp.is_unconditional() && i + 1 < n) out_s[k++] = uint32_t(i + 1);
          return k;
        case O::kLoopStart:
          if (c.loop_start.address() < n) out_s[k++] = c.loop_start.address();
          break;
        case O::kLoopEnd:
          if (c.loop_end.address() < n) out_s[k++] = c.loop_end.address();
          break;
        case O::kReturn:
          return 0;
        default:
          break;
      }
      if (i + 1 < n) out_s[k++] = uint32_t(i + 1);
      return k;
    };
    std::vector<uint8_t> state(n, 0);  // 0 unvisited, 1 on stack, 2 done
    std::vector<bool> reachable(n, false);
    bool cyclic = false;
    {
      std::vector<std::pair<uint32_t, int>> stack;
      if (n) { stack.push_back({0, 0}); state[0] = 1; reachable[0] = true; }
      while (!stack.empty()) {
        auto& [node, next] = stack.back();
        uint32_t s[2];
        const int ns = succ(node, s);
        if (next < ns) {
          const uint32_t t = s[next++];
          if (state[t] == 1) cyclic = true;
          else if (state[t] == 0) { state[t] = 1; reachable[t] = true; stack.push_back({t, 0}); }
        } else {
          state[node] = 2;
          stack.pop_back();
        }
      }
    }
    bool has_jumps = false, has_back = false;
    uint32_t back_edges = 0;
    for (size_t i = 0; i < n; ++i) {
      if (!reachable[i]) continue;
      const auto& c = cf[i];
      switch (c.opcode()) {
        case O::kCondJmp:
          has_jumps = true;
          if (c.cond_jmp.address() <= i) { has_back = true; ++back_edges; }
          break;
        case O::kLoopStart:
        case O::kLoopEnd:
          has_jumps = has_back = true;
          ++back_edges;
          break;
        case O::kCondCall:
          Err(Fmt("control flow opcode %u", uint32_t(c.opcode())));
          break;
        default:
          break;  // kReturn without calls is unreachable padding
      }
    }
    // Acyclic graphs with backward jumps (the compiler's if/else layout) run the dispatch loop at
    // most back_edges + 1 times, so it can be unrolled and implicit derivatives stay available.
    flow_loop = has_back && cyclic;
    if (has_back && cyclic) {
      Line("[loop] for (uint xe_iter = 0; xe_iter < 65536u; ++xe_iter) {");
      ++indent;
    } else if (has_back) {
      Line(Fmt("[unroll] for (uint xe_iter = 0; xe_iter < %uu; ++xe_iter) {", back_edges + 1));
      ++indent;
    }
    const char* jump_tail = has_back ? " continue;" : "";
    bool any_end = false;
    for (size_t i = 0; i < cf.size(); ++i) {
      const auto& c = cf[i];
      const O op = c.opcode();
      if (!reachable[i]) continue;
      if (op == O::kNop || op == O::kAlloc || op == O::kMarkVsFetchDone) continue;
      if (has_jumps) {
        Line(Fmt("if (pc <= %uu) {", uint32_t(i)));
        ++indent;
      }
      switch (op) {
        case O::kExec:
        case O::kExecEnd:
          dis += Fmt("  %zu: exec @%u x%u%s\n", i, c.exec.address(), c.exec.count(), op == O::kExecEnd ? " end" : "");
          EmitExec(c.exec.address(), c.exec.count(), c.exec.sequence());
          break;
        case O::kCondExec:
        case O::kCondExecEnd:
          dis += Fmt("  %zu: cexec b%u==%d @%u x%u%s\n", i, c.cond_exec.bool_address(), c.cond_exec.condition(),
                     c.cond_exec.address(), c.cond_exec.count(), op == O::kCondExecEnd ? " end" : "");
          Line(Fmt("if (XeBool(%uu) == %s) {", c.cond_exec.bool_address(), c.cond_exec.condition() ? "true" : "false"));
          ++indent;
          EmitExec(c.cond_exec.address(), c.cond_exec.count(), c.cond_exec.sequence());
          --indent;
          Line("}");
          break;
        case O::kCondExecPred:
        case O::kCondExecPredEnd:
        case O::kCondExecPredClean:
        case O::kCondExecPredCleanEnd:
          dis += Fmt("  %zu: pexec p0==%d @%u x%u\n", i, c.cond_exec_pred.condition(), c.cond_exec_pred.address(),
                     c.cond_exec_pred.count());
          Line(std::string("if (") + (c.cond_exec_pred.condition() ? "" : "!") + "p0) {");
          ++indent;
          EmitExec(c.cond_exec_pred.address(), c.cond_exec_pred.count(), c.cond_exec_pred.sequence());
          --indent;
          Line("}");
          break;
        case O::kCondJmp: {
          const auto& j = c.cond_jmp;
          std::string cond = j.is_unconditional() ? "true"
                             : j.is_predicated()  ? std::string(j.condition() ? "p0" : "!p0")
                                                  : Fmt("XeBool(%uu) == %s", j.bool_address(), j.condition() ? "true" : "false");
          dis += Fmt("  %zu: jmp %s -> %u\n", i, cond.c_str(), j.address());
          Line(Fmt("if (%s) { pc = %uu;%s }", cond.c_str(), j.address(), jump_tail));
          break;
        }
        case O::kLoopStart: {
          const auto& l = c.loop_start;
          dis += Fmt("  %zu: loop_start L%u skip-> %u\n", i, l.loop_id(), l.address());
          Line(Fmt("lc = int(xe_loop[%u].x); aL = int(xe_loop[%u].y);", l.loop_id(), l.loop_id()));
          Line(Fmt("if (lc <= 0) { pc = %uu;%s }", l.address(), jump_tail));
          break;
        }
        case O::kLoopEnd: {
          const auto& l = c.loop_end;
          dis += Fmt("  %zu: loop_end L%u -> %u\n", i, l.loop_id(), l.address());
          Line(Fmt("lc -= 1; aL += int(xe_loop[%u].z);", l.loop_id()));
          std::string brk = l.is_predicated_break() ? std::string(" && ") + (l.condition() ? "!p0" : "p0") : "";
          Line(Fmt("if (lc > 0%s) { pc = %uu;%s }", brk.c_str(), l.address(), jump_tail));
          break;
        }
        default:
          break;
      }
      if (uc::DoesControlFlowOpcodeEndShader(op)) {
        any_end = true;
        if (has_jumps) Line("pc = 0xFFFFu;");
      }
      if (has_jumps) {
        --indent;
        Line("}");
      }
      if (!has_jumps && uc::DoesControlFlowOpcodeEndShader(op)) break;
    }
    if (has_back) {
      Line("break;");
      --indent;
      Line("}");
    }
    if (!any_end) Err("no exec_end");
    return true;
  }

  std::string Assemble() {
    std::string s;
    s += CommonHlsl();
    s += "cbuffer XeConst : register(b0) { float4 c[256]; };\n";
    const uint32_t ntemp = out.max_temp < 16 ? 16 : out.max_temp;
    for (uint32_t i = 0; i < 32; ++i) {
      const char* t = nullptr;
      switch (out.tex_dim[i]) {
        case TexDim::k1D: t = "Texture1D"; break;
        case TexDim::k2D: t = "Texture2D"; break;
        case TexDim::k3D: t = "Texture3D"; break;
        case TexDim::kCube: t = "TextureCube"; break;
        default: break;
      }
      if (t) s += Fmt("%s<float4> tex%u : register(t%u);\n", t, i, i);
    }
    for (uint32_t i = 0; i < sampler_count && i < 16; ++i) s += Fmt("SamplerState smp%u : register(s%u);\n", i, i);
    if (!pixel)
      for (uint32_t i = 0; i < 96; ++i)
        if (out.vfetch_mask[i >> 5] & (1u << (i & 31))) s += Fmt("ByteAddressBuffer vb%u : register(t%u);\n", i, 32 + i);
    if (pixel) {
      s += "struct PsIn {\n  float4 pos : SV_Position;\n";
      for (int i = 0; i < 16; ++i) s += Fmt("  float4 v%d : TEXCOORD%d;\n", i, i);
      s += "  float2 sprite : XESPRITE;\n  bool front : SV_IsFrontFace;\n};\nstruct PsOut {\n";
      for (int i = 0; i < 4; ++i)
        if (out.color_targets & (1u << i)) s += Fmt("  float4 c%d : SV_Target%d;\n", i, i);
      if (out.writes_depth) s += "  float depth : SV_Depth;\n";
      if (!out.color_targets && !out.writes_depth) s += "  float4 c0 : SV_Target0;\n";
      s += "};\nPsOut main(PsIn input) {\n";
      s += Fmt("  float4 r[%u];\n  [unroll] for (uint ri = 0; ri < %uu; ++ri) r[ri] = 0.0;\n", ntemp, ntemp);
      const uint32_t ni = opt.interpolator_count > 16 ? 16 : opt.interpolator_count;
      for (uint32_t i = 0; i < ni; ++i) s += Fmt("  r[%u] = input.v%u;\n", i, i);
      if (opt.param_gen_reg >= 0 && opt.param_gen_reg < int(ntemp))
        s += Fmt("  r[%d] = float4(input.pos.xy * xe_misc.z - xe_misc.x * 0.5, input.sprite);\n  if (!input.front) r[%d].x = -r[%d].x;\n", opt.param_gen_reg, opt.param_gen_reg, opt.param_gen_reg);
      s += "  float4 oC0 = 0.0, oC1 = 0.0, oC2 = 0.0, oC3 = 0.0, oDepth4 = input.pos.zzzz;\n";
    } else {
      s += "struct VsOut {\n  float4 pos : SV_Position;\n";
      for (int i = 0; i < 16; ++i) s += Fmt("  float4 v%d : TEXCOORD%d;\n", i, i);
      s += "  float2 sprite : XESPRITE;\n  float psize : XEPSIZE;\n};\nVsOut main(uint vertex_id : SV_VertexID) {\n";
      s += Fmt("  float4 r[%u];\n  [unroll] for (uint ri = 0; ri < %uu; ++ri) r[ri] = 0.0;\n", ntemp, ntemp);
      s += "  r[0].x = (float)vertex_id;\n  uint vf_index = 0;\n";
      s += "  float4 o[16];\n  [unroll] for (uint oi = 0; oi < 16u; ++oi) o[oi] = 0.0;\n";
      // precise: depth-equal passes need bit-identical positions from different vertex shaders
      s += "  precise float4 oPos = float4(0.0, 0.0, 0.0, 1.0);\n  float4 oPts = 0.0;\n";
    }
    s += "  float ps = 0.0;\n  bool p0 = false;\n  int a0 = 0;\n  int aL = 0;\n  int lc = 0;\n  uint pc = 0;\n  float tex_lod = 0.0;\n  float4 dummy = 0.0;\n";
    s += body;
    if (pixel) {
      // Alpha to mask without MSAA (Xenos dithers the threshold per 2x2 pixel; after Xenia).
      s += "  if (xe_alpha.w != 0.0) {\n"
           "    uint2 pp = (uint2)input.pos.xy;\n"
           "    uint off = (asuint(xe_misc.y) >> ((((pp.x & 1u) << 1) | (pp.y & 1u)) * 2u)) & 3u;\n"
           "    if (!(oC0.w >= 1.0 - (float)off * 0.25)) discard;\n  }\n";
      s += "  if (xe_alpha.z != 0.0) {\n"
           "    float a = oC0.w, ref = xe_alpha.x; uint f = (uint)xe_alpha.y;\n"
           "    bool ok = f == 0 ? false : f == 1 ? a < ref : f == 2 ? a == ref : f == 3 ? a <= ref :\n"
           "              f == 4 ? a > ref : f == 5 ? a != ref : f == 6 ? a >= ref : true;\n"
           "    if (!ok) discard;\n  }\n";
      s += "  PsOut po;\n";
      for (int i = 0; i < 4; ++i)
        if (out.color_targets & (1u << i)) s += Fmt("  po.c%d = oC%d;\n", i, i);
      if (out.writes_depth) s += "  po.depth = oDepth4.x;\n";
      if (!out.color_targets && !out.writes_depth) s += "  po.c0 = oC0;\n";
      s += "  return po;\n}\n";
    } else {
      s += "  VsOut vo;\n";
      s += "  precise float4 p = oPos;\n";
      s += "  p.xyz = p.xyz * xe_ndc_scale.xyz + xe_ndc_offset.xyz * p.w;\n";
      s += "  vo.pos = p;\n";
      for (int i = 0; i < 16; ++i) s += Fmt("  vo.v%d = o[%d];\n", i, i);
      s += "  vo.sprite = 0.0;\n  vo.psize = oPts.x;\n  return vo;\n}\n";
    }
    s += "/* disassembly\n" + dis + "*/\n";
    return s;
  }
};

}  // namespace

const char* CommonHlsl() { return kCommon; }

TranslatedShader Translate(const uint32_t* ucode, size_t dword_count, bool pixel, const TranslateOptions& options) {
  Gen g{ucode, dword_count, pixel, options};
  g.out.pixel = pixel;
  g.Run();
  g.out.hlsl = g.Assemble();
  g.out.disassembly = g.dis;
  g.out.ok = g.out.error.empty();
  return std::move(g.out);
}

}  // namespace q4hle::xenos
