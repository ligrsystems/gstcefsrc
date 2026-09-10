#pragma once
#include <cstdint>
#include <climits>
namespace cef_gpu {
enum class Format { BGRA, RGBA, Unsupported };
struct Frame {
  int width, height, x, y, visible_width, visible_height;
  int plane_count, fd;
  uint32_t stride;
  uint64_t offset, size, modifier;
  Format format;
};
struct Layout { uint32_t drm_fourcc; };
constexpr uint32_t fourcc(char a,char b,char c,char d) {
  return uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24);
}
// CEF provides one packed RGB plane. Non-linear allocation sizes are opaque.
inline const char* validate(const Frame& f, int width, int height, Layout* out) {
  if (f.width <= 0 || f.height <= 0 || f.visible_width <= 0 || f.visible_height <= 0)
    return "invalid coded or visible dimensions";
  if (f.visible_width != width || f.visible_height != height)
    return "visible dimensions differ from negotiated output";
  if (f.x < 0 || f.y < 0 || f.x > f.width - f.visible_width || f.y > f.height - f.visible_height)
    return "visible rectangle exceeds coded dimensions";
  if (f.plane_count != 1 || f.fd < 0)
    return "expected one valid DMA-BUF plane";
  if (f.stride > INT_MAX || f.offset > INT_MAX || uint64_t(f.stride) < uint64_t(f.width) * 4)
    return "invalid EGL plane stride or offset";
  if (f.size == 0)
    return "empty DMA-BUF plane";
  if (f.modifier == 0 && f.size < uint64_t(f.stride) * (f.height - 1) + uint64_t(f.width) * 4)
    return "linear DMA-BUF plane is smaller than coded frame";
  switch (f.format) {
    case Format::BGRA: out->drm_fourcc = fourcc('A','R','2','4'); break;
    case Format::RGBA: out->drm_fourcc = fourcc('A','B','2','4'); break;
    default: return "unsupported CEF pixel format";
  }
  return nullptr;
}
}
