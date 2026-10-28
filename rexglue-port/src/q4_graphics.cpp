// q4 - GRAPHICS page in the game's own Options menu.
//
// The Xbox 360 Options menu has CONTROLS, AUDIO and GAME SETTINGS. When guis/mainmenu.gui is
// loaded (see the lexer hook in q4_quit_menu.cpp) a fourth entry, GRAPHICS, is added, styled like
// the others. It opens a settings page laid out like GAME SETTINGS with:
//
//   RESOLUTION         render resolution: 1280x720 x1..x4      (resolution_scale)
//   DISPLAY MODE       windowed / fullscreen                    (fullscreen)
//   V-SYNC             off / on                                 (q4hle_vsync in q4hle, vsync in q4)
//   ANTI-ALIASING      off / FXAA / FXAA extreme                (swap_post_effect)
//   TEXTURE FILTERING  off / 2x / 4x / 8x / 16x anisotropic     (anisotropic_override)
//   FPS COUNTER        off / on                                 (q4_show_fps)
//   3D MODE, 3D DEPTH  q4hle only: side-by-side stereo           (q4hle_stereo, q4hle_stereo_depth)
//   CRT SIMULATION     q4hle only: Blur Busters CRT beam simulator (q4hle_crt)
//
// Changes are made with left/right (or A) and take effect only when APPLY is selected; leaving
// the page with B discards them. The page talks to the host with console commands, handled here
// before the game's command system sees them (idCmdSystemLocal::ExecuteTokenizedString):
//   q4gfx_refresh         -> the host defines game cvars q4gfx_1..6 holding the current choices,
//                            which the page then reads with GetCvarValue; drops staged changes
//   q4gfx_stage <row> <i> -> remember choice i of row (not applied yet)
//   q4gfx_apply           -> apply the staged choices and save q4.toml
//   q4gfx_cancel          -> drop the staged choices
// These are the same settings as the F10 menu, so both stay in agreement.

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "q4_graphics.h"

namespace q4 {
std::filesystem::path ConfigPath();
}

// Native renderer V-Sync. Separate from the SDK's "vsync" (which paces Xenia's GPU and defaults
// to on) so that q4hle keeps presenting immediately unless the player turns it on. Defined here,
// in code both executables share, so that saving q4.toml from either one keeps it.
REXCVAR_DEFINE_BOOL(q4hle_vsync, false, "GPU",
                    "q4hle: wait for the display's vertical blank when presenting (no tearing, "
                    "more latency). Applies instantly.");

// Stereoscopic 3D in the native renderer (q4hle; see renderer.cpp "Stereoscopic 3D").
REXCVAR_DEFINE_INT32(q4hle_stereo, 0, "GPU",
                     "q4hle: stereoscopic 3D output. 0 = off, 1 = half side-by-side, 2 = full side-by-side");
REXCVAR_DEFINE_INT32(q4hle_stereo_depth, 2, "GPU", "q4hle: stereoscopic 3D depth strength, 1-3");

// Blur Busters CRT beam simulator in the native renderer (q4hle; see renderer.cpp "CRT beam
// simulation"). Needs a display of 120 Hz or more; keeps the game at 60 fps.
REXCVAR_DEFINE_INT32(q4hle_crt, 0, "GPU",
                     "q4hle: CRT beam simulation for clearer motion on 120 Hz+ displays. 0 = off, "
                     "1 = soft, 2 = medium, 3 = strong (clearer but dimmer)");

namespace q4gfx {
namespace {

std::mutex g_fs_mu;
std::function<void(bool)> g_fullscreen_handler;

#ifdef Q4_HLE
constexpr bool kLive = true;  // the native renderer applies resolution changes immediately
const char* const kVsyncCvar = "q4hle_vsync";
#else
constexpr bool kLive = false;
const char* const kVsyncCvar = "vsync";
#endif

struct Row {
  const char* label;
  const char* title;
  const char* desc;
  std::vector<const char*> choices;
};

const std::vector<Row>& Rows() {
  static const std::vector<Row> rows = {
      {"RESOLUTION", "RESOLUTION",
       kLive ? "Resolution the game renders at. Higher is sharper but slower."
             : "Resolution the game renders at. Higher is sharper but slower. Applies after a restart.",
       {"1280 x 720", "2560 x 1440", "3840 x 2160", "5120 x 2880"}},
      {"DISPLAY MODE", "DISPLAY MODE",
       kLive ? "Play in a window or fullscreen." : "Play in a window or fullscreen. Applies after a restart.",
       {"WINDOWED", "FULLSCREEN"}},
      {"V-SYNC", "V-SYNC", "Waits for the display refresh: no tearing, slightly more input latency.",
       {"OFF", "ON"}},
      {"ANTI-ALIASING", "ANTI-ALIASING", "Smooths jagged edges. FXAA is a fast post-process filter (not used in 3D mode).",
       {"OFF", "FXAA", "FXAA HIGH"}},
      {"TEXTURE FILTERING", "TEXTURE FILTERING", "Anisotropic filtering keeps textures sharp at steep angles.",
       {"OFF", "2X", "4X", "8X", "16X"}},
      {"FPS COUNTER", "FPS COUNTER", "Shows the frame rate in the top right corner. F9 also toggles it.",
       {"OFF", "ON"}},
#ifdef Q4_HLE
      {"3D MODE", "3D MODE",
       "Side-by-side 3D for 3D TVs, glasses and VR viewers. HALF SBS suits most 3D TVs.",
       {"OFF", "HALF SBS", "FULL SBS"}},
      {"3D DEPTH", "3D DEPTH", "How strong the 3D effect is.", {"LOW", "MEDIUM", "HIGH"}},
      {"CRT SIMULATION", "CRT SIMULATION",
       "Blur Busters CRT beam simulator: clearer motion. Needs a 120 Hz+ display; the game runs at "
       "60 fps (may need a restart). STRONG: clearer, dimmer.",
       {"OFF", "SOFT", "MEDIUM", "STRONG"}},
#endif
  };
  return rows;
}

bool CvarTrue(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

int CvarInt(const char* name, int def) {
  const std::string v = rex::cvar::GetFlagByName(name);
  if (v.empty()) return def;
  return std::atoi(v.c_str());
}

// Current choice of each row (index into Row::choices).
int CurrentChoice(size_t row) {
  switch (row) {
    case 0: return std::clamp(CvarInt("resolution_scale", 1), 1, 4) - 1;
    case 1: return CvarTrue("fullscreen") ? 1 : 0;
    case 2: return CvarTrue(kVsyncCvar) ? 1 : 0;
    case 3: {
      const std::string v = rex::cvar::GetFlagByName("swap_post_effect");
      return v == "fxaa" ? 1 : v == "fxaa_extreme" ? 2 : 0;
    }
    case 4: {
      const int a = CvarInt("anisotropic_override", 3);
      return a <= 1 ? 0 : std::min(a, 5) - 1;  // 2 -> 2x ... 5 -> 16x; -1/0/1 -> off
    }
    case 5: return CvarTrue("q4_show_fps") ? 1 : 0;
    case 6: return std::clamp(CvarInt("q4hle_stereo", 0), 0, 2);
    case 7: return std::clamp(CvarInt("q4hle_stereo_depth", 2), 1, 3) - 1;
    case 8: return std::clamp(CvarInt("q4hle_crt", 0), 0, 3);
  }
  return 0;
}

void Apply(size_t row, int choice) {
  const auto& rows = Rows();
  if (row >= rows.size()) return;
  choice = std::clamp(choice, 0, int(rows[row].choices.size()) - 1);
  switch (row) {
    case 0: rex::cvar::SetFlagByName("resolution_scale", std::to_string(choice + 1)); break;
    case 1: {
      const bool fs = choice == 1;
      rex::cvar::SetFlagByName("fullscreen", fs ? "true" : "false");
      std::lock_guard<std::mutex> lk(g_fs_mu);
      if (g_fullscreen_handler) g_fullscreen_handler(fs);
      break;
    }
    case 2: rex::cvar::SetFlagByName(kVsyncCvar, choice ? "true" : "false"); break;
    case 3: {
      static const char* const kAA[] = {"none", "fxaa", "fxaa_extreme"};
      rex::cvar::SetFlagByName("swap_post_effect", kAA[choice]);
      break;
    }
    case 4: rex::cvar::SetFlagByName("anisotropic_override", std::to_string(choice == 0 ? 0 : choice + 1)); break;
    case 5: rex::cvar::SetFlagByName("q4_show_fps", choice ? "true" : "false"); break;
    case 6: rex::cvar::SetFlagByName("q4hle_stereo", std::to_string(choice)); break;
    case 7: rex::cvar::SetFlagByName("q4hle_stereo_depth", std::to_string(choice + 1)); break;
    case 8: rex::cvar::SetFlagByName("q4hle_crt", std::to_string(choice)); break;
  }
  std::fprintf(stderr, "q4: graphics: %s = %s\n", rows[row].label, rows[row].choices[choice]);
}

// Choices selected on the page but not applied yet (game thread only).
std::vector<int> g_staged;  // per row, -1 = unchanged

void Stage(size_t row, int choice) {
  if (row >= Rows().size()) return;
  if (g_staged.size() != Rows().size()) g_staged.assign(Rows().size(), -1);
  g_staged[row] = choice;
}
void ClearStaged() { g_staged.assign(Rows().size(), -1); }

// Applies the staged choices; returns the rows that changed.
std::vector<size_t> ApplyStaged() {
  std::vector<size_t> done;
  for (size_t r = 0; r < g_staged.size(); ++r) {
    if (g_staged[r] < 0) continue;
    if (g_staged[r] != CurrentChoice(r)) Apply(r, g_staged[r]);
    done.push_back(r);
  }
  ClearStaged();
  if (!done.empty()) rex::cvar::SaveConfig(q4::ConfigPath());
  return done;
}

// ---------------------------------------------------------------------------------------------
// GUI text

struct Out {
  std::string s;
  int depth = 0;
  void L(const std::string& line) {
    s.append(size_t(depth), '\t');
    s += line;
    s += '\n';
  }
  void Open(const std::string& head) {
    L(head);
    L("{");
    ++depth;
  }
  void Close() {
    --depth;
    L("}");
  }
};

std::string Q(const std::string& v) { return "\"" + v + "\""; }
std::string N(size_t i) { return std::to_string(i); }

// gfx_valN text for every row; the focused row shows "<  value  >".
void EmitRefresh(Out& o) {
  const auto& rows = Rows();
  o.Open("onNamedEvent Refresh");
  for (size_t r = 0; r < rows.size(); ++r) {
    const std::string var = Q("gui::gfx_" + N(r + 1));
    const std::string val = "gfx_val" + N(r + 1) + "::text";
    for (size_t c = 0; c < rows[r].choices.size(); ++c) {
      o.L(std::string(c == 0 ? "if" : "} else if") + " (" + var + " == " + N(c) + ") {");
      ++o.depth;
      o.L("if (" + Q("gui::gfx_focus") + " == " + N(r + 1) + ") {");
      o.L("\tset " + Q(val) + " " + Q(std::string("<  ") + rows[r].choices[c] + "  >") + " ;");
      o.L("} else {");
      o.L("\tset " + Q(val) + " " + Q(rows[r].choices[c]) + " ;");
      o.L("}");
      --o.depth;
    }
    o.L("}");
  }
  const std::string ap = "gfx_val" + N(rows.size() + 1) + "::text";
  o.L("if (\"gui::gfx_dirty\" == 1) {");
  o.L("\tset " + Q(ap) + " \"CHANGES NOT APPLIED\" ;");
  o.L("} else {");
  o.L("\tset " + Q(ap) + " \"\" ;");
  o.L("}");
  o.Close();
}

// Step row r's choice by +1 / -1 (wrapping), tell the host, refresh the text.
void EmitStep(Out& o, size_t r, int dir) {
  const auto& rows = Rows();
  const size_t n = rows[r].choices.size();
  const std::string var = "gui::gfx_" + N(r + 1);
  for (size_t c = 0; c < n; ++c) {
    const size_t to = (c + n + size_t(dir + int(n))) % n;
    o.L(std::string(c == 0 ? "if" : "} else if") + " (" + Q(var) + " == " + N(c) + ") {");
    o.L("\tset " + Q(var) + " " + Q(N(to)) + " ;");
    o.L("\tconsoleCmd " + Q("q4gfx_stage " + N(r + 1) + " " + N(to)) + " ;");
  }
  o.L("}");
  o.L("set \"gui::gfx_dirty\" \"1\" ;");
  o.L("namedEvent " + Q("Menu_Adv_Video::Refresh") + " ;");
}

#ifdef Q4_HLE
constexpr int kRowY0 = 132, kRowDy = 25, kRowH = 28;  // nine settings
#else
constexpr int kRowY0 = 178, kRowDy = 29, kRowH = 31;
#endif
// Entries 0..rows-1 are the settings; entry `rows` is APPLY, a little lower.
int RowY(size_t r) { return kRowY0 + int(r) * kRowDy + (r >= Rows().size() ? 8 : 0); }
size_t Entries() { return Rows().size() + 1; }

std::string BuildPage() {
  const auto& rows = Rows();
  Out o;
  o.depth = 1;
  o.Open("windowDef Menu_Adv_Video");
  o.L("rect\t0,0,640,480");
  o.L("visible\t0");
  o.L("");
  o.Open("onJoyBack");
  o.L("consoleCmd \"q4gfx_cancel\" ;");
  o.L("set \"gui::gfx_dirty\" \"0\" ;");
  o.L("resettime \"anim_VideoOut\" \"0\" ;");
  o.L("set \"desktop::dest\" \"3\" ;");
  o.L("set \"desktop::active\" \"1\" ;");
  o.L("set \"cmd\" \"play main_menu_selection\" ;");
  o.Close();
  o.L("");
  EmitRefresh(o);
  o.L("");
  for (size_t r = 0; r < Entries(); ++r) {
    const int y = RowY(r);
    o.Open("windowDef gfx_c" + N(r + 1));
    o.L("rect\t47," + std::to_string(y + kRowH - 17) + ",18,18");
    o.L("visible\t1");
    o.L("background\t\"gfx/guis/mainmenu/corner\"");
    o.L("matscalex\t-1");
    o.L("matscaley\t-1");
    o.L("matcolor\t0.890,0.537,0,0");
    o.Close();
  }
  for (size_t r = 0; r < Entries(); ++r) {
    const bool apply = r == rows.size();
    const int y = RowY(r);
    const std::string btn = "gfx_btn" + N(r + 1);
    const std::string label = apply ? "APPLY" : rows[r].label;
    const std::string title = apply ? "APPLY" : rows[r].title;
    const std::string desc = apply ? "Applies the changes made above. Press B to leave without applying them." : rows[r].desc;
    o.Open("windowDef " + btn);
    o.L("rect\t50," + std::to_string(y) + ",236," + std::to_string(kRowH));
    o.L("visible\t1");
    o.L("background\t\"gfx/guis/mainmenu/b6_light\"");
    o.L("matcolor\t0.545,0.588,0.294,0.4");
    o.L("text\t" + Q(label));
    o.L("textscale\t0.29");
    o.L("font\t\"fonts/lowpixel\"");
    o.L("textalignx\t17");
    o.L("textaligny\t4");
    o.L("forecolor\t1,1,1,0.8");
    o.L("textstyle\t1");
    o.L("");
    o.Open("onMouseEnter");
    o.L("stoptransitions " + Q(btn) + " ;");
    o.L("set \"matcolor_w\" \"1\" ;");
    o.L("set " + Q("gfx_c" + N(r + 1) + "::matcolor_w") + " \"1\" ;");
    o.L("set " + Q("gfx_val" + N(r + 1) + "::forecolor") + " \"$desktop::white_8\" ;");
    o.L("set \"gui::gfx_focus\" " + Q(N(r + 1)) + " ;");
    o.L("namedEvent \"Menu_Adv_Video::Refresh\" ;");
    o.L("set \"gfx_t_title::text\" " + Q(title) + " ;");
    o.L("set \"gfx_t_desc::text\" " + Q(desc) + " ;");
    o.L("resettime \"gfx_t_desc\" \"0\" ;");
    o.L("set \"cmd\" \"play main_menu_mouseover\" ;");
    o.Close();
    o.L("");
    o.Open("onMouseExit");
    o.L("transition \"matcolor_w\" \"1\" \"0.4\" \"250\" ;");
    o.L("transition " + Q("gfx_c" + N(r + 1) + "::matcolor_w") + " \"1\" \"0\" \"250\" ;");
    o.L("set " + Q("gfx_val" + N(r + 1) + "::forecolor") + " \"$desktop::yellow_8\" ;");
    o.L("if (\"gui::gfx_focus\" == " + N(r + 1) + ") {");
    o.L("\tset \"gui::gfx_focus\" \"0\" ;");
    o.L("}");
    o.L("namedEvent \"Menu_Adv_Video::Refresh\" ;");
    o.Close();
    o.L("");
    if (apply) {
      o.Open("onAction");
      o.L("consoleCmd \"q4gfx_apply\" ;");
      o.L("set \"gui::gfx_dirty\" \"0\" ;");
      o.L("namedEvent \"Menu_Adv_Video::Refresh\" ;");
      o.L("set \"gfx_t_desc::text\" \"Settings applied.\" ;");
      o.L("resettime \"gfx_t_desc\" \"0\" ;");
      o.L("set \"cmd\" \"play main_menu_selection\" ;");
      o.Close();
      o.L("");
    } else {
      o.Open("onAction");
      EmitStep(o, r, +1);
      o.L("set \"cmd\" \"play main_menu_selection\" ;");
      o.Close();
      o.L("");
      o.Open("onJoyRight");
      EmitStep(o, r, +1);
      o.L("set \"cmd\" \"play main_menu_mouseover\" ;");
      o.Close();
      o.L("");
      o.Open("onJoyLeft");
      EmitStep(o, r, -1);
      o.L("set \"cmd\" \"play main_menu_mouseover\" ;");
      o.Close();
      o.L("");
    }
    o.Open("onJoyDown");
    o.L("setFocus " + Q("gfx_btn" + N((r + 1) % Entries() + 1)) + " ;");
    o.Close();
    o.L("");
    o.Open("onJoyUp");
    o.L("setFocus " + Q("gfx_btn" + N((r + Entries() - 1) % Entries() + 1)) + " ;");
    o.Close();
    o.L("");
    o.Close();
  }
  for (size_t r = 0; r < Entries(); ++r) {
    const int y = RowY(r);
    o.Open("windowDef gfx_val" + N(r + 1));
    o.L("rect\t300," + std::to_string(y + 5) + ",180,21");
    o.L("visible\t1");
    o.L("text\t\"\"");
    o.L("forecolor\t1,0.737,0.129,0.8");
    o.L("textscale\t0.29");
    o.L("font\t\"fonts/lowpixel\"");
    o.L("textstyle\t1");
    o.Close();
  }
  // Description panel (same look as the GAME SETTINGS one).
  o.Open("windowDef gfx_desc");
  o.L("rect\t370,146,319,202");
  o.L("visible\t1");
  o.Open("windowDef gfx_bg");
  o.L("rect\t25,135,294,53");
  o.L("visible\t1");
  o.L("background\t\"gfx/guis/mainmenu/b1_dark\"");
  o.L("matcolor\t1,1,1,0.4");
  o.Close();
  o.Open("windowDef gfx_bg2");
  o.L("rect\t25,54,294,94");
  o.L("visible\t1");
  o.L("background\t\"gfx/guis/mainmenu/b1_dark_fill\"");
  o.L("matcolor\t1,1,1,0.4");
  o.Close();
  o.Open("windowDef gfx_bg3");
  o.L("rect\t25,14,294,53");
  o.L("visible\t1");
  o.L("background\t\"gfx/guis/mainmenu/b1_dark\"");
  o.L("matcolor\t1,1,1,0.4");
  o.L("matscaley\t-1");
  o.Close();
  o.Open("windowDef gfx_corner");
  o.L("rect\t56,40,20,20");
  o.L("visible\t1");
  o.L("background\t\"gfx/guis/mainmenu/corner\"");
  o.L("matcolor\t0.560,0.6,0.282,0.4");
  o.L("matscaley\t-1");
  o.Close();
  o.Open("windowDef gfx_t_title");
  o.L("rect\t76,40,200,36");
  o.L("visible\t1");
  o.L("forecolor\t0.835,1,0.654,0.8");
  o.L("font\t\"fonts/lowpixel\"");
  o.L("text\t" + Q(rows[0].title));
  o.L("textscale\t0.26");
  o.Close();
  o.Open("windowDef gfx_t_desc");
  o.L("rect\t61,70,161,92");
  o.L("visible\t1");
  o.L("forecolor\t1,0.745,0.137,0.8");
  o.L("font\t\"fonts/lowpixel\"");
  o.L("text\t" + Q(rows[0].desc));
  o.L("textscale\t0.28");
  o.Open("onTime 0");
  o.L("transition \"forecolor\" \"$desktop::white\" \"$desktop::yellow\" \"250\" ;");
  o.L("transition \"gfx_t_title::forecolor\" \"$desktop::white\" \"0.835,1,0.654,0.8\" \"250\" ;");
  o.Close();
  o.Close();
  o.Close();
  o.Close();  // Menu_Adv_Video
  return o.s;
}

// anim_VideoIn / anim_VideoOut (anim_OptionsOut already starts anim_VideoIn for dest 16).
std::string BuildAnims() {
  const auto& rows = Rows();
  const size_t n = Entries();  // settings + APPLY
  Out o;
  o.depth = 2;
  o.Open("windowDef anim_VideoIn");
  o.L("rect\t0,0,0,0");
  o.L("visible\t1");
  o.L("notime\t1");
  o.L("");
  o.Open("onTime 0");
  for (size_t r = 1; r <= n; ++r) {
    o.L("set " + Q("gfx_c" + N(r) + "::matcolor_w") + " \"0\" ;");
    o.L("set " + Q("gfx_btn" + N(r) + "::matcolor_w") + " \"0\" ;");
    o.L("set " + Q("gfx_btn" + N(r) + "::forecolor_w") + " \"0\" ;");
    o.L("set " + Q("gfx_val" + N(r) + "::forecolor") + " \"$desktop::yellow_8\" ;");
    o.L("set " + Q("gfx_val" + N(r) + "::forecolor_w") + " \"0\" ;");
  }
  for (const char* w : {"gfx_bg", "gfx_bg2", "gfx_bg3", "gfx_corner"}) o.L("set " + Q(std::string(w) + "::matcolor_w") + " \"0\" ;");
  o.L("set \"gfx_t_title::forecolor_w\" \"0\" ;");
  o.L("set \"gfx_t_desc::forecolor_w\" \"0\" ;");
  o.L("set \"gfx_t_title::text\" " + Q(rows[0].title) + " ;");
  o.L("set \"gfx_t_desc::text\" " + Q(rows[0].desc) + " ;");
  for (size_t r = 1; r <= rows.size(); ++r)
    o.L("set \"cmd\" " + Q("GetCvarValue q4gfx_" + N(r) + " gfx_" + N(r)) + " ;");
  o.L("set \"gui::gfx_dirty\" \"0\" ;");
  o.L("set \"gui::gfx_focus\" \"1\" ;");
  o.L("namedEvent \"Menu_Adv_Video::Refresh\" ;");
  o.Close();
  o.L("");
  o.Open("onTime +50");
  o.L("setFocus \"gfx_btn1\" ;");
  o.L("transition \"grad3::matcolor_w\" \"0\" \"0.4\" \"250\" ;");
  o.L("set \"gui::playerprofileVis\" \"0\" ;");
  o.L("set \"gui::configcontrollerVis\" \"0\" ;");
  o.L("set \"gui::advsettingsVis\" \"1\" ;");
  o.L("set \"gfx_c1::matcolor_w\" \"1\" ;");
  for (size_t r = 1; r <= n; ++r) {
    o.L("transition " + Q("gfx_btn" + N(r) + "::matcolor_w") + " \"0\" " + Q(r == 1 ? "1" : "0.4") + " \"250\" ;");
    o.L("transition " + Q("gfx_btn" + N(r) + "::forecolor_w") + " \"0\" \"0.8\" \"250\" ;");
    o.L("transition " + Q("gfx_val" + N(r) + "::forecolor_w") + " \"0\" \"1\" \"250\" ;");
  }
  for (const char* w : {"gfx_bg", "gfx_bg2", "gfx_bg3", "gfx_corner"})
    o.L("transition " + Q(std::string(w) + "::matcolor_w") + " \"0\" \"0.4\" \"250\" ;");
  o.L("transition \"gfx_t_title::forecolor_w\" \"0\" \"0.8\" \"250\" ;");
  o.L("transition \"gfx_t_desc::forecolor_w\" \"0\" \"0.8\" \"250\" ;");
  o.L("set \"Menu_Adv_Video::visible\" \"1\" ;");
  o.L("resettime \"anim_btnsIn\" \"0\" ;");
  o.L("namedEvent \"p_img_main::DisplayHeader\" ;");
  o.L("set \"desktop::curr\" \"16\" ;");
  o.Close();
  o.L("");
  o.Open("onTime +100");
  o.L("set \"desktop::active\" \"0\" ;");
  o.L("namedEvent \"Menu_Adv_Video::Refresh\" ;");
  o.Close();
  o.L("");
  o.Close();

  o.Open("windowDef anim_VideoOut");
  o.L("rect\t0,0,0,0");
  o.L("visible\t1");
  o.L("notime\t1");
  o.L("");
  o.Open("onTime 0");
  o.L("set \"cmd\" \"play main_menu_selection\" ;");
  o.L("resettime \"anim_btnsOut\" \"0\" ;");
  o.L("resettime \"anim_TitlesOut\" \"0\" ;");
  for (size_t r = 1; r <= n; ++r) {
    o.L("transition " + Q("gfx_c" + N(r) + "::matcolor_w") + " \"1\" \"0\" \"250\" ;");
    o.L("transition " + Q("gfx_btn" + N(r) + "::matcolor_w") + " \"0.4\" \"0\" \"250\" ;");
    o.L("transition " + Q("gfx_btn" + N(r) + "::forecolor_w") + " \"0.8\" \"0\" \"250\" ;");
    o.L("transition " + Q("gfx_val" + N(r) + "::forecolor_w") + " \"1\" \"0\" \"250\" ;");
  }
  for (const char* w : {"gfx_bg", "gfx_bg2", "gfx_bg3", "gfx_corner"})
    o.L("transition " + Q(std::string(w) + "::matcolor_w") + " \"0.4\" \"0\" \"250\" ;");
  o.L("transition \"gfx_t_title::forecolor_w\" \"0.8\" \"0\" \"250\" ;");
  o.L("transition \"gfx_t_desc::forecolor_w\" \"0.8\" \"0\" \"250\" ;");
  o.L("transition \"grad3::matcolor_w\" \"0.4\" \"0\" \"250\" ;");
  o.Close();
  o.L("");
  o.Open("onTime 250");
  o.L("set \"Menu_Adv_Video::visible\" \"0\" ;");
  o.L("resettime \"anim_OptionsIn\" \"0\" ;");
  o.Close();
  o.L("");
  o.Close();
  return o.s;
}

// The GRAPHICS entry in Menu_Options (hit area over the shared b_btn5/t_btn5/corner_btn5).
std::string BuildOptionsButton() {
  Out o;
  o.depth = 2;
  o.Open("windowDef opt_btn5");
  o.L("rect\t56,294,328,23");
  o.L("visible\t1");
  o.L("");
  o.Open("onMouseEnter");
  o.L("stoptransitions \"opt_btn5\" ;");
  o.L("set \"b_btn5::matcolor_w\" \"1\" ;");
  o.L("transition \"t_btn5::forecolor\" \"$desktop::white_8\" \"$desktop::orange\" \"0\" ;");
  o.L("transition \"corner_btn5::matcolor\" \"$desktop::corner\" \"$desktop::orange\" \"0\" ;");
  o.L("set \"t_btn5::textscale\" \"0.34\" ;");
  o.L("set \"cmd\" \"play main_menu_mouseover\" ;");
  o.Close();
  o.L("");
  o.Open("onMouseExit");
  o.L("transition \"b_btn5::matcolor_w\" \"1\" \"0.4\" \"300\" ;");
  o.L("transition \"t_btn5::forecolor\" \"$desktop::orange\" \"$desktop::white_8\" \"300\" ;");
  o.L("transition \"corner_btn5::matcolor\" \"$desktop::orange\" \"$desktop::corner\" \"300\" ;");
  o.L("transition \"t_btn5::textscale\" \"0.34\" \"0.33\" \"300\" ;");
  o.Close();
  o.L("");
  o.Open("onAction");
  o.L("set \"desktop::active\" \"1\" ;");
  o.L("consoleCmd \"q4gfx_refresh\" ;");
  for (size_t r = 1; r <= Rows().size(); ++r)
    o.L("set \"cmd\" " + Q("GetCvarValue q4gfx_" + N(r) + " gfx_" + N(r)) + " ;");
  o.L("resettime \"anim_OptionsOut\" \"0\" ;");
  o.L("set \"desktop::dest\" \"16\" ;");
  o.L("set \"cmd\" \"play main_menu_selection\" ;");
  o.Close();
  o.L("");
  o.Open("onJoyUp");
  o.L("setFocus \"opt_btn4\" ;");
  o.Close();
  o.L("");
  o.Open("onJoyDown");
  o.L("setFocus \"opt_btn2\" ;");
  o.Close();
  o.L("");
  o.Close();
  return o.s;
}

}  // namespace

void SetFullscreenHandler(std::function<void(bool)> handler) {
  std::lock_guard<std::mutex> lk(g_fs_mu);
  g_fullscreen_handler = std::move(handler);
}

bool PatchMainMenu(std::string& text) {
  std::string s = text;
  if (s.find("windowDef opt_btn5") != std::string::npos || s.find("windowDef Menu_Adv_Video") != std::string::npos)
    return false;  // already there
  const bool crlf = s.find("\r\n") != std::string::npos;

  // Bounds of "windowDef <name>" up to the next "windowDef".
  auto find_def = [&](const std::string& name, size_t from) -> size_t {
    const std::string key = "windowDef " + name;
    size_t p = s.find(key, from);
    while (p != std::string::npos) {
      const char c = p + key.size() < s.size() ? s[p + key.size()] : '\n';
      if (c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '{') return p;
      p = s.find(key, p + 1);
    }
    return std::string::npos;
  };
  auto next_def = [&](size_t p) {
    const size_t q = s.find("windowDef", p + 9);
    return q == std::string::npos ? s.size() : q;
  };
  auto fix_nl = [&](std::string t) {
    if (!crlf) return t;
    std::string r;
    r.reserve(t.size() + t.size() / 16);
    for (char c : t) {
      if (c == '\n') r += '\r';
      r += c;
    }
    return r;
  };
  // Start of the line containing position p (insert before its indentation).
  auto line_start = [&](size_t p) {
    while (p > 0 && s[p - 1] != '\n') --p;
    return p;
  };
  auto replace_once = [&](size_t b, size_t e, std::string_view from, std::string_view to) {
    const size_t p = s.find(from, b);
    if (p == std::string::npos || p + from.size() > e) return false;
    s.replace(p, from.size(), to);
    return true;
  };

  // --- Menu_Options: wire opt_btn5 into up/down navigation, then add it -------------------------
  const size_t mo = find_def("Menu_Options", 0);
  if (mo == std::string::npos) return false;
  size_t b = find_def("opt_btn2", mo);
  if (b == std::string::npos) return false;
  {
    const size_t up = s.find("onJoyUp", b);
    if (up == std::string::npos || up > next_def(b)) return false;
    if (!replace_once(up, next_def(b), "setFocus \"opt_btn4\"", "setFocus \"opt_btn5\"")) return false;
  }
  b = find_def("opt_btn4", mo);
  if (b == std::string::npos) return false;
  {
    const size_t dn = s.find("onJoyDown", b);
    if (dn == std::string::npos || dn > next_def(b)) return false;
    if (!replace_once(dn, next_def(b), "setFocus \"opt_btn2\"", "setFocus \"opt_btn5\"")) return false;
  }
  const size_t after_opt = find_def("Menu_Opt_Controller", mo);
  if (after_opt == std::string::npos) return false;
  // opt_btn4's body ends with "\t\t}\r\n\t}" before Menu_Opt_Controller; insert after opt_btn4's
  // closing brace, i.e. before the line holding Menu_Options' closing brace.
  {
    size_t close_menu = s.rfind('}', after_opt);           // Menu_Options' "}"
    if (close_menu == std::string::npos || close_menu < b) return false;
    s.insert(line_start(close_menu), fix_nl(BuildOptionsButton()));
  }

  // --- anim_OptionsIn: show the 4th entry and label it; focus it when coming back -------------
  size_t a = find_def("anim_OptionsIn", 0);
  if (a == std::string::npos) return false;
  {
    const size_t e = next_def(a);
    for (const char* w : {"b_btn5", "t_btn5", "corner_btn5"}) {
      const std::string from = std::string("set \"") + w + "::visible\" \"0\" ;";
      const std::string to = std::string("set \"") + w + "::visible\" \"1\" ;";
      if (!replace_once(a, e, from, to)) return false;
    }
    const std::string lbl = "set \"t_btn4::text\" \"#str_202173\" ;";
    const size_t p = s.find(lbl, a);
    if (p == std::string::npos || p > next_def(a)) return false;
    s.insert(p + lbl.size(), fix_nl("\n\t\t\t\tset \"t_btn5::text\" \"GRAPHICS\" ;"));
    const std::string c17 = "else if (\"desktop::curr\" == 17) {";
    const size_t q = s.find(c17, a);
    if (q == std::string::npos || q > next_def(a)) return false;
    s.insert(line_start(q), fix_nl("\t\t\t\telse if (\"desktop::curr\" == 16) {\n"
                                   "\t\t\t\t\tset \"desktop::btnhighlight\" \"5\" ;\n"
                                   "\t\t\t\t\tsetFocus \"opt_btn5\" ;\n"
                                   "\t\t\t\t}\n"));
  }

  // --- header text for dest 16 (VIDEO -> GRAPHICS) ------------------------------------------
  {
    const size_t h = s.find("onNamedEvent DisplayHeader");
    if (h == std::string::npos) return false;
    const std::string d16 = "else if (\"desktop::dest\" == 16) {";
    const size_t p = s.find(d16, h);
    if (p == std::string::npos || p > h + 20000) return false;
    if (!replace_once(p, p + 200, "\"#str_202172\"", "\"GRAPHICS\"")) return false;
  }

  // --- the page and its animations ------------------------------------------------------------
  const size_t credits = find_def("Menu_Credits", 0);
  if (credits == std::string::npos) return false;
  s.insert(line_start(credits), fix_nl(BuildPage()));
  const size_t gsin = find_def("anim_GameSettingsIn", 0);
  if (gsin == std::string::npos) return false;
  s.insert(line_start(gsin), fix_nl(BuildAnims()));

  text = std::move(s);
  return true;
}

}  // namespace q4gfx

#ifndef Q4QUIT_STANDALONE_TEST

namespace {

uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
bool GuestPtrOk(uint32_t a) { return a >= 0x10000 && a < 0xE0000000u; }

std::string GuestStr(uint8_t* base, uint32_t a) {
  if (!GuestPtrOk(a)) return {};
  const char* p = reinterpret_cast<const char*>(base + a);
  return std::string(p, strnlen(p, 256));
}

}  // namespace

REX_EXTERN(sub_8234F870);  // Mem_Alloc(size, tag)
REX_EXTERN(sub_8234FA20);  // Mem_Free(ptr)
REX_EXTERN(sub_8216B580);  // idCmdSystemLocal::InsertCommandText(this, text)

namespace {

// Runs `text` right after the command being executed (idCmdSystemLocal::InsertCommandText).
void InsertCommands(PPCContext& ctx, uint8_t* base, uint32_t cmd_system, const std::string& text) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = text.size() + 1;
  ctx.r4.u64 = 3;
  sub_8234F870(ctx, base);
  const uint32_t buf = ctx.r3.u32;
  if (GuestPtrOk(buf)) {
    std::memcpy(base + buf, text.c_str(), text.size() + 1);
    ctx.r3.u64 = cmd_system;
    ctx.r4.u64 = buf;
    sub_8216B580(ctx, base);
    ctx.r3.u64 = buf;
    sub_8234FA20(ctx, base);
  }
  ctx = saved;
}

std::string SetCvarsText() {
  std::string cmd;
  for (size_t r = 0; r < q4gfx::Rows().size(); ++r)
    cmd += "set q4gfx_" + std::to_string(r + 1) + " " + std::to_string(q4gfx::CurrentChoice(r)) + "\n";
  return cmd;
}

bool g_cvars_defined = false;

}  // namespace

// idCmdSystemLocal::ExecuteTokenizedString( const idCmdArgs &args ): r3 = this, r4 = args
// (argc at +0, argv[] from +4). Runs on the game thread.
REX_EXTERN(__imp__sub_8216B400);
extern "C" REX_FUNC(sub_8216B400) {
  const uint32_t self = ctx.r3.u32, args = ctx.r4.u32;
  if (!g_cvars_defined && GuestPtrOk(self)) {
    // First command (startup configs): define the cvars the GRAPHICS page reads.
    g_cvars_defined = true;
    InsertCommands(ctx, base, self, SetCvarsText());
  }
  if (GuestPtrOk(args)) {
    const uint32_t argc = Ld32(base, args);
    if (argc >= 1 && argc < 64) {
      const std::string a0 = GuestStr(base, Ld32(base, args + 4));
      if (a0.size() > 6 && (a0.compare(0, 6, "q4gfx_") == 0 || a0.compare(0, 6, "Q4GFX_") == 0)) {
        std::string cmd = a0;
        for (char& c : cmd) c = char(std::tolower(static_cast<unsigned char>(c)));
        const auto arg_int = [&](uint32_t i) { return std::atoi(GuestStr(base, Ld32(base, args + 4 + 4 * i)).c_str()); };
        if ((cmd == "q4gfx_stage" || cmd == "q4gfx_set") && argc >= 3) {
          const int row = arg_int(1), choice = arg_int(2);
          if (row >= 1 && size_t(row) <= q4gfx::Rows().size()) q4gfx::Stage(size_t(row - 1), choice);
          if (cmd == "q4gfx_set") q4gfx::ApplyStaged();  // immediate form (console use)
        } else if (cmd == "q4gfx_apply") {
          q4gfx::ApplyStaged();
          InsertCommands(ctx, base, self, SetCvarsText());  // the page reads these next time
        } else if (cmd == "q4gfx_cancel") {
          q4gfx::ClearStaged();
        } else if (cmd == "q4gfx_refresh") {
          q4gfx::ClearStaged();
          InsertCommands(ctx, base, self, SetCvarsText());
        }
        return;
      }
    }
  }
  __imp__sub_8216B400(ctx, base);
}

#endif  // Q4QUIT_STANDALONE_TEST
