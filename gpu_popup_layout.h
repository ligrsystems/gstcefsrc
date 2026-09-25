#pragma once
#include <cstdint>
#include <algorithm>
namespace cef_gpu {
struct PopupRect { int x, y, width, height; };
struct PopupClip { int x, y, width, height, source_x, source_y; };
inline bool same_rect(const PopupRect& a, const PopupRect& b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
inline PopupClip clip_popup(int view_width, int view_height, const PopupRect& rect) {
  if (view_width <= 0 || view_height <= 0 || rect.width <= 0 || rect.height <= 0) return {};
  const int64_t left = std::max<int64_t>(0, rect.x);
  const int64_t top = std::max<int64_t>(0, rect.y);
  const int64_t right = std::min<int64_t>(view_width, int64_t(rect.x) + rect.width);
  const int64_t bottom = std::min<int64_t>(view_height, int64_t(rect.y) + rect.height);
  if (right <= left || bottom <= top) return {};
  return {int(left), int(top), int(right - left), int(bottom - top),
      int(left - rect.x), int(top - rect.y)};
}
}
namespace cef_gpu {
struct PopupUpdate { bool clear_pixels, publish; };
struct PopupState {
  bool visible = false, has_pixels = false;
  PopupRect rect{};
  PopupUpdate Update(bool show, const PopupRect& next) {
    if (show == visible && same_rect(next, rect)) return {};
    const bool clear = !show || next.width != rect.width || next.height != rect.height;
    // A state barrier also republishes the retained clean view without popup pixels.
    const bool publish = true;
    visible = show;
    rect = next;
    if (clear) has_pixels = false;
    return {clear, publish};
  }
};
}
