// q4 - in-window settings menu (opened with F10). See q4_menu.h.

#include "q4_menu.h"

#include <rex/cvar.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

struct Choice {
  const char* label;
  const char* value;
};

std::string GetCvar(const char* name) { return rex::cvar::GetFlagByName(name); }

void SetCvar(const char* name, const std::string& value) { rex::cvar::SetFlagByName(name, value); }

bool GetBool(const char* name) { return GetCvar(name) == "true"; }

int IndexOf(const Choice* choices, int n, const std::string& cur, int fallback) {
  for (int i = 0; i < n; ++i) {
    if (cur == choices[i].value) return i;
  }
  return fallback;
}

// Draws a combo for a cvar. Returns true when the value changed.
bool CvarCombo(const char* label, const char* cvar, const Choice* choices, int n, int fallback,
               const char* hint) {
  const int cur = IndexOf(choices, n, GetCvar(cvar), fallback);
  bool changed = false;
  if (ImGui::BeginCombo(label, choices[cur].label)) {
    for (int i = 0; i < n; ++i) {
      const bool sel = (i == cur);
      if (ImGui::Selectable(choices[i].label, sel)) {
        SetCvar(cvar, choices[i].value);
        changed = true;
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (hint && *hint) {
    ImGui::TextDisabled("%s", hint);
  }
  ImGui::Spacing();
  return changed;
}

}  // namespace

Q4MenuDialog::Q4MenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks)
    : rex::ui::ImGuiDialog(drawer), callbacks_(std::move(callbacks)) {}

Q4MenuDialog::~Q4MenuDialog() = default;

void Q4MenuDialog::RequestClose() { close_requested_ = true; }

void Q4MenuDialog::OnClose() {
  if (callbacks_.on_closed) callbacks_.on_closed();
}

void Q4MenuDialog::OnDraw(ImGuiIO& io) {
  const ImVec2 display = io.DisplaySize;
  const float w = std::min(760.0f, display.x * 0.8f);
  const float h = std::min(560.0f, display.y * 0.8f);
  ImGui::SetNextWindowPos(ImVec2((display.x - w) * 0.5f, (display.y - h) * 0.5f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.94f);

  bool open = true;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("QUAKE 4 - SETTINGS  (F10 to close)", &open, flags)) {
    if (ImGui::BeginTabBar("##q4tabs")) {
      if (ImGui::BeginTabItem("Video")) {
        DrawVideo();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Audio")) {
        DrawAudio();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Advanced")) {
        DrawAdvanced();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (restart_pending_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "Some changes need a restart to take effect.");
      ImGui::SameLine();
      if (ImGui::Button("Apply & Restart")) {
        if (callbacks_.persist_config) callbacks_.persist_config();
        if (callbacks_.request_restart) callbacks_.request_restart();
      }
    }
    if (ImGui::Button("Close")) close_requested_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Quit to desktop") && callbacks_.on_quit) callbacks_.on_quit();
  }
  ImGui::End();

  if (!open) close_requested_ = true;
  if (close_requested_) Close();
}

void Q4MenuDialog::DrawVideo() {
  ImGui::Spacing();

  // Fullscreen applies live (the owner defers the window change to the UI thread).
  bool fs = callbacks_.get_fullscreen ? callbacks_.get_fullscreen() : GetBool("fullscreen");
  if (ImGui::Checkbox("Fullscreen", &fs)) {
    if (callbacks_.request_fullscreen) callbacks_.request_fullscreen(fs);
    SetCvar("fullscreen", fs ? "true" : "false");
    if (callbacks_.persist_config) callbacks_.persist_config();
  }
  ImGui::Spacing();

  bool vsync = GetBool("vsync");
  if (ImGui::Checkbox("V-Sync", &vsync)) {
    SetCvar("vsync", vsync ? "true" : "false");
    if (callbacks_.persist_config) callbacks_.persist_config();
  }
  ImGui::TextDisabled("(on: frame timing follows your display for even motion; off: free-running,");
  ImGui::TextDisabled(" lowest latency but may tear or judder. Applies instantly.)");
  ImGui::Spacing();

  if (callbacks_.get_show_fps && callbacks_.set_show_fps) {
    bool show = callbacks_.get_show_fps();
    if (ImGui::Checkbox("Show FPS counter (F9)", &show)) {
      callbacks_.set_show_fps(show);
    }
    ImGui::TextDisabled("(top-right corner: frames per second, frame time and the longest recent frame)");
    ImGui::Spacing();
  }
  if (callbacks_.get_hide_cursor && callbacks_.set_hide_cursor) {
    bool hide = callbacks_.get_hide_cursor();
    if (ImGui::Checkbox("Hide mouse cursor while playing", &hide)) {
      callbacks_.set_hide_cursor(hide);
    }
    ImGui::TextDisabled("(the cursor reappears briefly when you move the mouse, and while this menu is open)");
    ImGui::Spacing();
  }
  ImGui::Separator();
  ImGui::Spacing();

  // Windowed size (used when not fullscreen; read at startup).
  static const struct {
    const char* label;
    int w, h;
  } kSizes[] = {{"1280 x 720", 1280, 720},
                {"1600 x 900", 1600, 900},
                {"1920 x 1080", 1920, 1080},
                {"2560 x 1440", 2560, 1440},
                {"3840 x 2160", 3840, 2160}};
  const int cw = std::atoi(GetCvar("window_width").c_str());
  const int ch = std::atoi(GetCvar("window_height").c_str());
  int idx = -1;
  for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i) {
    if (kSizes[i].w == cw && kSizes[i].h == ch) idx = i;
  }
  char cur_label[48];
  if (idx >= 0) {
    std::snprintf(cur_label, sizeof cur_label, "%s", kSizes[idx].label);
  } else if (cw > 0 && ch > 0) {
    std::snprintf(cur_label, sizeof cur_label, "%d x %d", cw, ch);
  } else {
    std::snprintf(cur_label, sizeof cur_label, "Default");
  }
  if (ImGui::BeginCombo("Window size", cur_label)) {
    for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i) {
      if (ImGui::Selectable(kSizes[i].label, i == idx)) {
        SetCvar("window_width", std::to_string(kSizes[i].w));
        SetCvar("window_height", std::to_string(kSizes[i].h));
        if (callbacks_.persist_config) callbacks_.persist_config();
        restart_pending_ = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("(window size for windowed mode; needs a restart)");
  ImGui::Spacing();

  // Internal resolution: supersampling of the 1280x720 render targets.
  static const Choice kIRes[] = {{"720p (native, 1x)", "1"},
                                 {"1440p (2x)", "2"},
                                 {"2160p / 4K (3x)", "3"},
                                 {"2880p (4x)", "4"}};
  if (CvarCombo("Internal resolution", "resolution_scale", kIRes, IM_ARRAYSIZE(kIRes), 0,
                "(resolution the 3D scene is rendered at; needs a restart)")) {
    if (callbacks_.persist_config) callbacks_.persist_config();
    restart_pending_ = true;
  }

  static const Choice kAA[] = {
      {"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA (extreme)", "fxaa_extreme"}};
  if (CvarCombo("Anti-aliasing", "swap_post_effect", kAA, IM_ARRAYSIZE(kAA), 0,
                "(post-process anti-aliasing; applies instantly)")) {
    if (callbacks_.persist_config) callbacks_.persist_config();
  }

  static const Choice kAniso[] = {
      {"Off (bilinear)", "0"}, {"2x", "1"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}};
  if (CvarCombo("Texture filtering", "anisotropic_override", kAniso, IM_ARRAYSIZE(kAniso), 2,
                "(anisotropic filtering; applies instantly)")) {
    if (callbacks_.persist_config) callbacks_.persist_config();
  }
}

void Q4MenuDialog::DrawAudio() {
  ImGui::Spacing();
  bool mute = GetBool("audio_mute");
  if (ImGui::Checkbox("Mute audio", &mute)) {
    SetCvar("audio_mute", mute ? "true" : "false");
    if (callbacks_.persist_config) callbacks_.persist_config();
  }
  ImGui::TextDisabled("(more audio options will be added later)");
}

void Q4MenuDialog::DrawAdvanced() {
  ImGui::Spacing();
  ImGui::TextUnformatted("Game location");
  const std::string loc = callbacks_.get_game_location ? callbacks_.get_game_location() : std::string();
  ImGui::TextDisabled("%s", loc.empty() ? "(unknown)" : loc.c_str());
  if (ImGui::Button("Locate ISO / game folder...") && callbacks_.change_game_location) {
    callbacks_.change_game_location();
    restart_pending_ = true;
  }
  ImGui::TextDisabled("(pick your Quake 4 ISO or its default.xex; takes effect after a restart)");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextWrapped(
      "More options will be added here later. The full list of engine settings is also "
      "available in the F4 settings overlay.");
  ImGui::Spacing();
  ImGui::TextDisabled("Config file: q4.toml (next to q4.exe)");
}
