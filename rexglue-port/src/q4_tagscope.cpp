// q4 - thread-private memory-tag scope (hooks generated into q4_tag_hooks.toml by gen_tag.py).
//
// The game keeps the current memory tag scope in one global (0x830863EC): functions push a stack
// record {tag, previous} and set the global to it, and pop with global = global->previous. That
// assumes one thread. When the renderer back end runs on its own thread (q4hle, Q4_RB_THREAD), its
// pushes and pops would interleave with the game thread's and leave the global pointing at dead
// stack records. Every load and store of that global is hooked: on a thread that called
// q4tag::UsePrivateScope() it uses a per-thread slot instead; elsewhere the original runs.

#include <rex/ppc/context.h>
#include <cstdint>

namespace q4tag {
thread_local bool t_private = false;
thread_local uint32_t t_slot = 0;
void UsePrivateScope() { t_private = true; t_slot = 0; }
}  // namespace q4tag

bool Q4Tag_L_r4(PPCRegister& r4) { if (!q4tag::t_private) return false; r4.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r6(PPCRegister& r6) { if (!q4tag::t_private) return false; r6.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r7(PPCRegister& r7) { if (!q4tag::t_private) return false; r7.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r8(PPCRegister& r8) { if (!q4tag::t_private) return false; r8.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r9(PPCRegister& r9) { if (!q4tag::t_private) return false; r9.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r10(PPCRegister& r10) { if (!q4tag::t_private) return false; r10.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r11(PPCRegister& r11) { if (!q4tag::t_private) return false; r11.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r23(PPCRegister& r23) { if (!q4tag::t_private) return false; r23.u64 = q4tag::t_slot; return true; }
bool Q4Tag_L_r29(PPCRegister& r29) { if (!q4tag::t_private) return false; r29.u64 = q4tag::t_slot; return true; }
bool Q4Tag_S_r3(PPCRegister& r3) { if (!q4tag::t_private) return false; q4tag::t_slot = r3.u32; return true; }
bool Q4Tag_S_r4(PPCRegister& r4) { if (!q4tag::t_private) return false; q4tag::t_slot = r4.u32; return true; }
bool Q4Tag_S_r6(PPCRegister& r6) { if (!q4tag::t_private) return false; q4tag::t_slot = r6.u32; return true; }
bool Q4Tag_S_r7(PPCRegister& r7) { if (!q4tag::t_private) return false; q4tag::t_slot = r7.u32; return true; }
bool Q4Tag_S_r8(PPCRegister& r8) { if (!q4tag::t_private) return false; q4tag::t_slot = r8.u32; return true; }
bool Q4Tag_S_r9(PPCRegister& r9) { if (!q4tag::t_private) return false; q4tag::t_slot = r9.u32; return true; }
bool Q4Tag_S_r10(PPCRegister& r10) { if (!q4tag::t_private) return false; q4tag::t_slot = r10.u32; return true; }
bool Q4Tag_S_r11(PPCRegister& r11) { if (!q4tag::t_private) return false; q4tag::t_slot = r11.u32; return true; }
bool Q4Tag_S_r23(PPCRegister& r23) { if (!q4tag::t_private) return false; q4tag::t_slot = r23.u32; return true; }
bool Q4Tag_S_r29(PPCRegister& r29) { if (!q4tag::t_private) return false; q4tag::t_slot = r29.u32; return true; }

// ---- back-end thread mid-function hooks (q4_rb_hooks.toml) ----
namespace q4rb {
// set by q4hle's back-end thread (src/hle/rb_thread.cpp) while it runs; always false in q4.exe
bool g_active = false;
}  // namespace q4rb

bool Q4Rb_OverlayRealloc() { return q4rb::g_active; }

// ---- front-end helper threads: per-thread shadow volume state (q4_rb_hooks.toml) ----
namespace q4fe {
// byte offset from the shadow code's static block (0x82FE7928) to this thread's copy; 0 = shared
thread_local int32_t t_delta = 0;
}  // namespace q4fe

void Q4Fe_Rebase_r9(PPCRegister& r9) { if (q4fe::t_delta) r9.s64 = int32_t(r9.u32 + uint32_t(q4fe::t_delta)); }
void Q4Fe_Rebase_r10(PPCRegister& r10) { if (q4fe::t_delta) r10.s64 = int32_t(r10.u32 + uint32_t(q4fe::t_delta)); }
void Q4Fe_Rebase_r11(PPCRegister& r11) { if (q4fe::t_delta) r11.s64 = int32_t(r11.u32 + uint32_t(q4fe::t_delta)); }
