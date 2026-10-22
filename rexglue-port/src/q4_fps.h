// q4 - FPS counter overlay (top-right corner). Toggled with F9 or from the F10 menu.

#pragma once

#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

#include <chrono>
#include <cstdint>

// Shows the game's real frame rate (frames the game hands to the display per second), not the
// host window's paint rate. It never closes itself; the owner deletes it at shutdown.
class Q4FpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit Q4FpsOverlay(rex::ui::ImGuiDrawer* drawer);
  ~Q4FpsOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::chrono::steady_clock::time_point last_time_{};
  uint64_t last_frames_ = 0;
  double fps_ = 0.0;
  double frame_ms_ = 0.0;
  double worst_ms_ = 0.0;  // longest frame in the last measurement interval
  uint64_t prev_frame_total_ = 0;
  std::chrono::steady_clock::time_point prev_frame_time_{};
  double interval_worst_ = 0.0;
};
