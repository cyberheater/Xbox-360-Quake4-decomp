// q4 - "QUIT GAME" on the title-screen main menu.
//
// The Xbox 360 main menu (guis/mainmenu.gui) has no way to leave the game. Two pieces add one:
//
//  1. When the game's lexer loads guis/mainmenu.gui, the text is patched in memory (the game files
//     on disk are not touched): a "QUIT GAME" button (the game's own localized #str_202378) is
//     added under CREDITS, styled like the other entries, wired into up/down navigation, and runs
//     the console command "quit" when selected.
//     The GRAPHICS options page (q4_graphics.cpp) is added to the same text.
//  2. The console command "quit" (Com_Quit_f) is overridden to close the port the same way the F10
//     menu's "Quit to desktop" does (config is saved on every change; q4::HardExit).
//
// If any anchor in the GUI is not found (a different game version) the file is left unchanged.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "q4_graphics.h"

namespace q4 {
[[noreturn]] void HardExit();
}

namespace q4quit {

// Returns the patched GUI text, or nullopt if the file does not look like the expected one.
std::optional<std::string> PatchMainMenu(std::string_view src) {
  std::string s(src);
  const bool crlf = s.find("\r\n") != std::string::npos;
  const char* nl = crlf ? "\r\n" : "\n";
  auto lines = [&](std::initializer_list<const char*> ls) {
    std::string out;
    for (const char* l : ls) {
      out += l;
      out += nl;
    }
    return out;
  };

  if (s.find("main_btn7") != std::string::npos) return std::nullopt;  // already has one

  // Locate a window section: from "windowDef <name>" up to the next "windowDef" at or after `from`.
  auto section = [&](const std::string& name, size_t lo, size_t hi, size_t& b, size_t& e) {
    const std::string key = "windowDef " + name;
    size_t p = s.find(key, lo);
    while (p != std::string::npos && p < hi) {
      const char c = s[p + key.size()];
      if (c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '{') break;  // whole name
      p = s.find(key, p + 1);
    }
    if (p == std::string::npos || p >= hi) return false;
    size_t q = s.find("windowDef", p + key.size());
    if (q == std::string::npos || q > hi) q = hi;
    b = p;
    e = q;
    return true;
  };
  auto replace_in = [&](size_t b, size_t e, std::string_view from, std::string_view to) {
    size_t p = s.find(from, b);
    if (p == std::string::npos || p + from.size() > e) return false;
    s.replace(p, from.size(), to);
    return true;
  };

  // --- Menu_Main: navigation into and out of the new button ---------------------------------
  size_t mm_b, mm_e;
  if (!section("Menu_Main", 0, s.size(), mm_b, mm_e)) return std::nullopt;
  // Menu_Main ends where the next top-level menu starts.
  size_t mm_end = s.find("windowDef Menu_SPSaveGame", mm_b);
  if (mm_end == std::string::npos) return std::nullopt;

  size_t b, e;
  // main_btn2 (LOAD GAME) and main_btn3 (CAMPAIGN): "up" from the top entry wraps to the bottom.
  if (!section("main_btn2", mm_b, mm_end, b, e)) return std::nullopt;
  if (!replace_in(s.find("onJoyUp", b), e, "setFocus \"main_btn6\"", "setFocus \"main_btn7\""))
    return std::nullopt;
  if (!section("main_btn3", mm_b, mm_end, b, e)) return std::nullopt;
  if (!replace_in(s.find("onJoyUp", b), e, "setFocus \"main_btn6\"", "setFocus \"main_btn7\""))
    return std::nullopt;
  // main_btn6 (CREDITS): "down" goes to QUIT GAME instead of wrapping.
  if (!section("main_btn6", mm_b, mm_end, b, e)) return std::nullopt;
  {
    const size_t d = s.find("onJoyDown {", b);
    const size_t u = s.find("onJoyUp {", b);
    if (d == std::string::npos || u == std::string::npos || d > u || u > e) return std::nullopt;
    s.replace(d, u - d,
              lines({"onJoyDown {", "\t\t\t\tsetFocus \"main_btn7\" ;", "\t\t\t}", ""}) + "\t\t\t");
  }

  // --- Menu_Main: the button (visuals laid out like the shared b_/t_/corner_btn6, one row down) --
  mm_end = s.find("windowDef Menu_SPSaveGame", mm_b);
  size_t ins;
  if (!section("main_t_failed", mm_b, mm_end, ins, e)) return std::nullopt;
  const std::string button = lines({
      "windowDef q4quit_b",
      "\t\t{",
      "\t\t\trect\t24,330,413,52",
      "\t\t\tvisible\t1",
      "\t\t\tbackground\t\"gfx/guis/mainmenu/b1_dark\"",
      "\t\t\tmatcolor\t1,1,1,0.4",
      "\t\t}",
      "\t\twindowDef q4quit_t",
      "\t\t{",
      "\t\t\trect\t69,342,407,23",
      "\t\t\tvisible\t1",
      "\t\t\tforecolor\t1,1,1,0.8",
      "\t\t\ttext\t\"#str_202378\"",
      "\t\t\ttextscale\t0.33",
      "\t\t\tfont\t\"fonts/marine\"",
      "\t\t}",
      "\t\twindowDef q4quit_corner",
      "\t\t{",
      "\t\t\trect\t56,350,12,12",
      "\t\t\tvisible\t1",
      "\t\t\tbackground\t\"gfx/guis/mainmenu/corner\"",
      "\t\t\tmatcolor\t0.564,0.603,0.286,0.4",
      "\t\t}",
      "\t\twindowDef main_btn7",
      "\t\t{",
      "\t\t\trect\t56,330,328,23",
      "\t\t\tvisible\t1",
      "",
      "\t\t\tonMouseEnter {",
      "\t\t\t\tstoptransitions \"main_btn7\" ;",
      "\t\t\t\tset \"q4quit_b::matcolor_w\" \"1\" ;",
      "\t\t\t\ttransition \"q4quit_t::forecolor\" \"$desktop::white_8\" \"$desktop::orange\" \"0\" ;",
      "\t\t\t\ttransition \"q4quit_corner::matcolor\" \"$desktop::corner\" \"$desktop::orange\" \"0\" ;",
      "\t\t\t\tset \"q4quit_t::textscale\" \"0.34\" ;",
      "\t\t\t\tset \"cmd\" \"play main_menu_mouseover\" ;",
      "\t\t\t}",
      "",
      "\t\t\tonMouseExit {",
      "\t\t\t\ttransition \"q4quit_b::matcolor_w\" \"1\" \"0.4\" \"300\" ;",
      "\t\t\t\ttransition \"q4quit_t::forecolor\" \"$desktop::orange\" \"$desktop::white_8\" \"300\" ;",
      "\t\t\t\ttransition \"q4quit_corner::matcolor\" \"$desktop::orange\" \"$desktop::corner\" \"300\" ;",
      "\t\t\t\ttransition \"q4quit_t::textscale\" \"0.34\" \"0.33\" \"300\" ;",
      "\t\t\t}",
      "",
      "\t\t\tonAction {",
      "\t\t\t\tset \"cmd\" \"play main_menu_selection\" ;",
      "\t\t\t\tconsoleCmd \"quit\" ;",
      "\t\t\t}",
      "",
      "\t\t\tonJoyDown {",
      "\t\t\t\tif (\"gui::lastSaveAvailable\" == 1) {",
      "\t\t\t\t\tsetFocus \"main_btn2\" ;",
      "\t\t\t\t} else {",
      "\t\t\t\t\tsetFocus \"main_btn3\" ;",
      "\t\t\t\t}",
      "\t\t\t}",
      "",
      "\t\t\tonJoyUp {",
      "\t\t\t\tsetFocus \"main_btn6\" ;",
      "\t\t\t}",
      "",
      "\t\t}",
  }) + "\t\t";
  s.insert(ins, button);

  // --- anim_MainIn: start the new entry hidden and fade it in with the others ---------------
  size_t a_b, a_e;
  if (!section("anim_MainIn", 0, s.size(), a_b, a_e)) return std::nullopt;
  {
    const std::string_view vis = "set \"Menu_Main::visible\" \"1\" ;";
    size_t p = s.find(vis, a_b);
    if (p == std::string::npos || p > a_e) return std::nullopt;
    const std::string add =
        std::string(nl) +
        lines({"\t\t\t\tset \"q4quit_b::matcolor_w\" \"0\" ;",
               "\t\t\t\tset \"q4quit_t::forecolor\" \"1,1,1,0\" ;",
               "\t\t\t\tset \"q4quit_t::textscale\" \"0.33\" ;",
               "\t\t\t\tset \"q4quit_corner::matcolor\" \"0.564,0.603,0.286,0\" ;"});
    s.insert(p + vis.size(), add.substr(0, add.size() - std::strlen(nl)));
    section("anim_MainIn", 0, s.size(), a_b, a_e);
    const std::string_view btns = "namedEvent \"p_btns_g::BtnsIn\" ;";
    p = s.find(btns, a_b);
    if (p == std::string::npos || p > a_e) return std::nullopt;
    const std::string fade =
        std::string(nl) +
        lines({"\t\t\t\ttransition \"q4quit_b::matcolor_w\" \"0\" \"0.4\" \"450\" ;",
               "\t\t\t\ttransition \"q4quit_t::forecolor_w\" \"0\" \"0.8\" \"450\" ;",
               "\t\t\t\ttransition \"q4quit_corner::matcolor_w\" \"0\" \"0.4\" \"450\" ;"});
    s.insert(p + btns.size(), fade.substr(0, fade.size() - std::strlen(nl)));
  }
  return s;
}

}  // namespace q4quit

#ifndef Q4QUIT_STANDALONE_TEST

namespace {

uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
bool GuestPtrOk(uint32_t a) { return a >= 0x10000 && a < 0xE0000000u; }

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i) {
    char c = s[s.size() - suffix.size() + i];
    if (c == '\\') c = '/';
    if (std::tolower(static_cast<unsigned char>(c)) != suffix[i]) return false;
  }
  return true;
}

}  // namespace

REX_EXTERN(sub_8234F870);  // idlib heap: Mem_Alloc(size, tag)
REX_EXTERN(sub_8234FA20);  // idlib heap: Mem_Free(ptr)

// idLexer::LoadFile( const char *filename, bool OSPath ). The lexer keeps its filename in an idStr
// at +4 (data pointer at +8) and the text buffer it owns at +0x28 (buffer), +0x2C (script_p),
// +0x30 (end_p), +0x34 (lastScript_p), +0x44 (length).
REX_EXTERN(__imp__sub_823491C8);
extern "C" REX_FUNC(sub_823491C8) {
  const uint32_t lexer = ctx.r3.u32;
  __imp__sub_823491C8(ctx, base);
  if (ctx.r3.u32 != 1 || !GuestPtrOk(lexer)) return;

  const uint32_t name_p = Ld32(base, lexer + 8);
  if (!GuestPtrOk(name_p)) return;
  const char* name = reinterpret_cast<const char*>(base + name_p);
  if (!EndsWithNoCase(std::string_view(name, strnlen(name, 512)), "guis/mainmenu.gui")) return;

  const uint32_t buf = Ld32(base, lexer + 0x28);
  const uint32_t len = Ld32(base, lexer + 0x44);
  if (!GuestPtrOk(buf) || len == 0 || len > (64u << 20)) return;
  // The lexer must own the buffer (LoadFile allocated it) and not have started reading.
  if (base[lexer + 0x24] != 1 || Ld32(base, lexer + 0x2C) != buf) return;

  std::string text(reinterpret_cast<const char*>(base + buf), len);
  bool changed = false;
  if (auto quit = q4quit::PatchMainMenu(text)) {
    text = std::move(*quit);
    changed = true;
    std::fprintf(stderr, "q4: added Quit Game to the main menu\n");
  } else {
    std::fprintf(stderr, "q4: main menu layout not recognized; Quit Game not added\n");
  }
  if (q4gfx::PatchMainMenu(text)) {
    changed = true;
    std::fprintf(stderr, "q4: added Graphics to the options menu\n");
  } else {
    std::fprintf(stderr, "q4: options menu layout not recognized; Graphics not added\n");
  }
  if (!changed) return;
  const std::string* patched = &text;

  // Allocate the new text on the guest heap with the same tag the lexer uses, then free the old.
  const PPCRegister r3 = ctx.r3, r4 = ctx.r4;
  ctx.r3.u64 = patched->size() + 1;
  ctx.r4.u64 = 3;
  sub_8234F870(ctx, base);
  const uint32_t nb = ctx.r3.u32;
  if (!GuestPtrOk(nb)) {
    ctx.r3 = r3;
    ctx.r4 = r4;
    return;
  }
  std::memcpy(base + nb, patched->data(), patched->size());
  base[nb + patched->size()] = 0;
  ctx.r3.u64 = buf;
  sub_8234FA20(ctx, base);

  const uint32_t nl = static_cast<uint32_t>(patched->size());
  St32(base, lexer + 0x28, nb);
  St32(base, lexer + 0x2C, nb);
  St32(base, lexer + 0x34, nb);
  St32(base, lexer + 0x30, nb + nl);
  St32(base, lexer + 0x44, nl);
  ctx.r3 = r3;
  ctx.r4 = r4;
}

// Com_Quit_f (console command "quit"): close the port.
extern "C" REX_FUNC(sub_82171718) {
  std::fflush(nullptr);
  q4::HardExit();
}

#endif  // Q4QUIT_STANDALONE_TEST

// ---------------------------------------------------------------------------------------------
// Debug-assert breakpoints. 0x82855158 is the game's import thunk for the kernel's DbgBreakPoint,
// reached only from the engine's debug asserts (about 37 call sites, e.g. a draw in
// 0x822DE140 that finds no index data). On a retail Xbox 360 with no debugger attached these do
// nothing and the game carries on; the SDK's DbgBreakPoint executes a host breakpoint instead,
// which ends the process. Treat them as on retail: log the call site (first few) and continue.
extern "C" REX_FUNC(sub_82855158) {
  (void)base;
  static int logged = 0;
  if (logged < 20) {
    ++logged;
    std::fprintf(stderr, "q4: guest debug assert ignored (DbgBreakPoint) called from %08X\n", uint32_t(ctx.lr));
  }
}
