// q4 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <filesystem>
#include <string>

#include "q4_fps.h"
#include "q4_menu.h"

REXCVAR_DECLARE(bool, q4_hide_cursor);
REXCVAR_DECLARE(bool, q4_show_fps);

namespace q4 {
// Relaunch this executable as a fresh process (implemented in q4_hooks.cpp).
void LaunchSelfDetached();
// q4.toml next to the executable (same file the SDK loads at startup).
std::filesystem::path ConfigPath();
// Terminate the process immediately (config is already saved). Used for restart/quit because
// the graceful guest teardown can corrupt the heap on exit.
[[noreturn]] void HardExit();
// A valid game location is an extracted folder containing default.xex, or a Quake 4 .iso file.
bool IsValidGameLocation(const std::filesystem::path& p);
// Shows a file dialog (optionally preceded by a message) to locate the ISO / default.xex.
std::optional<std::filesystem::path> PromptForGameLocation(const wchar_t* intro_message);
}  // namespace q4

class Q4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Q4App>(new Q4App(ctx, "q4", PPCImageConfig));
  }

  // Boot defaults. These run before q4.toml is loaded, so anything saved from the
  // settings menu overrides them.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (rex::cvar::GetFlagByName("gpu_plugin").empty()) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    // Quake 4 opens a LAN (system link) UDP socket at startup even in single player. Keep it on
    // loopback so Windows Firewall does not prompt. q4.toml can override this.
    rex::cvar::SetFlagByName("net_bind_loopback_only", "true");
  }

  // Resolve where the game is. Order: the saved/explicit path, then the usual places. If nothing
  // valid is found, ask the user to locate their ISO (or default.xex) and remember the choice.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    rex::PathConfig cfg = defaults;
    std::vector<std::filesystem::path> candidates;
    const std::string saved = rex::cvar::GetFlagByName("game_data_root");
    if (!saved.empty()) candidates.emplace_back(saved);
    if (!defaults.game_data_root.empty()) candidates.push_back(defaults.game_data_root);
    candidates.emplace_back("C:\\Quake4\\game");
    candidates.push_back(q4::ConfigPath().parent_path() / "game");
    for (const auto& c : candidates) {
      if (q4::IsValidGameLocation(c)) {
        cfg.game_data_root = c;
        return cfg;
      }
    }
    auto chosen = q4::PromptForGameLocation(
        L"Quake 4 could not find your game files.\n\n"
        L"This project does not include the game. Next, choose YOUR Quake 4 (Xbox 360) ISO, or "
        L"default.xex from the extracted game. The location is remembered.");
    if (!chosen) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    rex::cvar::SetFlagByName("game_data_root", chosen->string());
    PersistConfig();
    cfg.game_data_root = *chosen;
    return cfg;
  }

  // Register the F10 settings-menu keybind once the ImGui drawer exists.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (window()) window()->SetTitle("Quake 4");
    if (window()) window()->SetCursorAutoHideDelayMs(1500);
    ApplyCursorVisibility();
    rex::ui::RegisterBind("bind_q4_menu", "F10", "Quake 4 settings menu", [this] { ToggleMenu(); });
    rex::ui::RegisterBind("bind_q4_fps", "F9", "Toggle the FPS counter", [this] { ToggleFpsCounter(); });
    fps_overlay_ = new Q4FpsOverlay(drawer);
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_q4_menu");
    rex::ui::UnregisterBind("bind_q4_fps");
    if (fps_overlay_) {
      delete fps_overlay_;
      fps_overlay_ = nullptr;
    }
    if (menu_) {
      delete menu_;  // removes itself from the drawer in its destructor
      menu_ = nullptr;
    }
  }

 private:
  void PersistConfig() { rex::cvar::SaveConfig(q4::ConfigPath()); }

  void ToggleFpsCounter() {
    rex::cvar::SetFlagByName("q4_show_fps", REXCVAR_GET(q4_show_fps) ? "false" : "true");
    PersistConfig();
  }

  // Hide the cursor while playing; keep it visible while the F10 menu is open.
  void ApplyCursorVisibility() {
    if (!window()) return;
    const bool hide = REXCVAR_GET(q4_hide_cursor) && !menu_;
    window()->SetCursorVisibility(hide ? rex::ui::Window::CursorVisibility::kAutoHidden
                                       : rex::ui::Window::CursorVisibility::kVisible);
  }

  void ToggleMenu() {
    if (menu_) {
      menu_->RequestClose();  // on_closed clears menu_
      return;
    }
    Q4MenuDialog::Callbacks cb;
    cb.on_closed = [this] {
      menu_ = nullptr;
      app_context().CallInUIThreadDeferred([this] { ApplyCursorVisibility(); });
    };
    cb.get_hide_cursor = [] { return REXCVAR_GET(q4_hide_cursor); };
    cb.get_show_fps = [] { return REXCVAR_GET(q4_show_fps); };
    cb.set_show_fps = [this](bool on) {
      rex::cvar::SetFlagByName("q4_show_fps", on ? "true" : "false");
      PersistConfig();
    };
    cb.set_hide_cursor = [this](bool hide) {
      rex::cvar::SetFlagByName("q4_hide_cursor", hide ? "true" : "false");
      PersistConfig();
    };
    cb.get_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    cb.request_fullscreen = [this](bool v) {
      rex::cvar::SetFlagByName("fullscreen", v ? "true" : "false");
      PersistConfig();
      // Apply between frames on the UI thread, never from inside the paint.
      app_context().CallInUIThreadDeferred([this, v] {
        if (window()) window()->SetFullscreen(v);
      });
    };
    cb.persist_config = [this] { PersistConfig(); };
    cb.get_game_location = [this] { return game_data_root().string(); };
    cb.change_game_location = [this] {
      // Native dialogs must not run inside the ImGui paint; run it between frames.
      app_context().CallInUIThreadDeferred([this] {
        auto chosen = q4::PromptForGameLocation(nullptr);
        if (chosen) {
          rex::cvar::SetFlagByName("game_data_root", chosen->string());
          PersistConfig();
        }
      });
    };
    cb.request_restart = [this] {
      // Persisted already by the menu. Relaunch, then exit hard (see q4::HardExit).
      app_context().CallInUIThreadDeferred([] {
        q4::LaunchSelfDetached();
        q4::HardExit();
      });
    };
    cb.on_quit = [this] {
      PersistConfig();
      app_context().CallInUIThreadDeferred([] { q4::HardExit(); });
    };
    menu_ = new Q4MenuDialog(imgui_drawer(), std::move(cb));
    ApplyCursorVisibility();
  }

  Q4MenuDialog* menu_ = nullptr;  // non-owning; self-deletes via the drawer
  Q4FpsOverlay* fps_overlay_ = nullptr;  // always-on overlay; deleted at shutdown
};
