// q4 - in-window settings menu (opened with F10).
//
// Video / audio / advanced options that map onto rexglue cvars. Changes that can
// apply live do so immediately; the rest are flagged "restart" and applied via a
// quick relaunch.

#pragma once

#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

#include <functional>
#include <string>

class Q4MenuDialog : public rex::ui::ImGuiDialog {
 public:
  struct Callbacks {
    std::function<void()> on_closed;               // clear owner pointer
    std::function<bool()> get_fullscreen;          // live window state
    std::function<void(bool)> request_fullscreen;  // deferred window change
    std::function<void()> persist_config;          // write q4.toml
    std::function<void()> request_restart;         // relaunch with saved config
    std::function<void()> on_quit;                 // quit to desktop
    std::function<std::string()> get_game_location;  // current ISO / game folder
    std::function<void()> change_game_location;      // pick a different ISO / folder
    std::function<bool()> get_hide_cursor;           // hide the mouse cursor while playing
    std::function<void(bool)> set_hide_cursor;
    std::function<bool()> get_show_fps;              // FPS counter on/off
    std::function<void(bool)> set_show_fps;
  };

  Q4MenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks);
  ~Q4MenuDialog() override;

  void RequestClose();

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void DrawVideo();
  void DrawAudio();
  void DrawAdvanced();

  Callbacks callbacks_;
  bool close_requested_ = false;
  bool restart_pending_ = false;  // a restart-only option changed
};
