// q4 - FPS counter overlay. See q4_fps.h.

#include "q4_fps.h"

#include <rex/cvar.h>
#include <rex/system/xtimer.h>

#include <algorithm>

REXCVAR_DECLARE(bool, q4_show_fps);

Q4FpsOverlay::Q4FpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

Q4FpsOverlay::~Q4FpsOverlay() = default;

void Q4FpsOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(q4_show_fps)) {
    // Keep the measurement baseline fresh so the first reading after turning it on is right.
    last_time_ = {};
    prev_frame_time_ = {};
    return;
  }

  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  const uint64_t frames = rex::system::GuestFrameCount();

  if (last_time_ == clock::time_point{}) {
    last_time_ = now;
    last_frames_ = frames;
    prev_frame_total_ = frames;
    prev_frame_time_ = now;
    interval_worst_ = 0.0;
  }

  // Longest gap between game frames since the last reading (spots hitches the average hides).
  if (frames != prev_frame_total_) {
    const double gap_ms = std::chrono::duration<double, std::milli>(now - prev_frame_time_).count();
    interval_worst_ = std::max(interval_worst_, gap_ms);
    prev_frame_total_ = frames;
    prev_frame_time_ = now;
  }

  const double dt = std::chrono::duration<double>(now - last_time_).count();
  if (dt >= 0.5) {
    fps_ = static_cast<double>(frames - last_frames_) / dt;
    frame_ms_ = fps_ > 0.0 ? 1000.0 / fps_ : 0.0;
    worst_ms_ = interval_worst_;
    interval_worst_ = 0.0;
    last_time_ = now;
    last_frames_ = frames;
  }

  // Scale for 4K (ImGui's default font is tiny there).
  const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 24.0f * scale, 24.0f * scale), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.45f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("##q4_fps_overlay", nullptr, flags)) {
    ImGui::SetWindowFontScale(scale * 2.3f);
    ImVec4 colour(0.45f, 0.95f, 0.30f, 1.0f);  // green: at or near 60
    if (fps_ < 55.0) colour = ImVec4(1.0f, 0.85f, 0.25f, 1.0f);
    if (fps_ < 40.0) colour = ImVec4(1.0f, 0.35f, 0.25f, 1.0f);
    ImGui::TextColored(colour, "%.0f FPS", fps_);
    ImGui::TextDisabled("%.1f ms  (worst %.0f)", frame_ms_, worst_ms_);
  }
  ImGui::End();
}
