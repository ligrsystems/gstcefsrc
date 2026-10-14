// Portable checks for gst_cef_unpremultiply_bgra. No CEF or GStreamer needed:
//   c++ -std=c++14 -O2 -Wall -Wextra -Werror -I. tests/alpha_test.cc -o alpha-test
#include "gstcefalpha.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;

#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      std::fprintf (stderr, "FAIL: " __VA_ARGS__); \
      std::fprintf (stderr, "\n");               \
      failures++;                                \
    }                                            \
  } while (0)

// cudacompositor / compositor "over": dst' = src * a + dst * (1 - a).
static double
straight_over (double src, double a, double dst)
{
  return src * a + dst * (1.0 - a);
}

// For every premultiplied value Chromium can paint, the converted pixel must
// composite to the same result as premultiplied source-over.
static void
test_round_trip_matches_premultiplied_over (void)
{
  const double backgrounds[] = { 0, 64, 128, 200, 255 };
  for (int a = 1; a < 256; a++) {
    for (int p = 0; p <= a; p++) {
      uint8_t in[4] = { (uint8_t) p, (uint8_t) p, (uint8_t) p, (uint8_t) a };
      uint8_t out[4];
      gst_cef_unpremultiply_bgra (out, in, 1);
      CHECK (out[3] == a, "alpha changed a=%d", a);
      for (double bg : backgrounds) {
        const double af = a / 255.0;
        const double got = straight_over (out[0] / 255.0, af, bg / 255.0) * 255.0;
        const double want = p + bg * (1.0 - af);
        CHECK (std::fabs (got - want) <= 0.5, "a=%d p=%d bg=%.0f got=%.3f want=%.3f",
            a, p, bg, got, want);
      }
    }
  }
}

static void
test_fast_paths_and_channels (void)
{
  // B, G, R, A per pixel: opaque, transparent, 50 % white, 50 % orange.
  const uint8_t in[] = {
    10, 20, 30, 255,
    0, 0, 0, 0,
    128, 128, 128, 128,
    0, 82, 128, 128,
  };
  uint8_t out[sizeof in];
  gst_cef_unpremultiply_bgra (out, in, 4);
  const uint8_t want[] = {
    10, 20, 30, 255,
    0, 0, 0, 0,
    255, 255, 255, 128,
    0, 163, 255, 128,
  };
  for (size_t i = 0; i < sizeof want; i++)
    CHECK (out[i] == want[i], "byte %zu got %d want %d", i, out[i], want[i]);
}

// Chromium never paints colour above alpha, but a bad value must clamp, not wrap.
static void
test_out_of_range_clamps (void)
{
  const uint8_t in[4] = { 200, 200, 200, 100 };
  uint8_t out[4];
  gst_cef_unpremultiply_bgra (out, in, 1);
  CHECK (out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 100,
      "clamp got %d %d %d %d", out[0], out[1], out[2], out[3]);
}

static double
time_frame_ms (const std::vector<uint8_t> & frame)
{
  std::vector<uint8_t> out (frame.size ());
  const int runs = 20;
  auto start = std::chrono::steady_clock::now ();
  for (int i = 0; i < runs; i++)
    gst_cef_unpremultiply_bgra (out.data (), frame.data (), frame.size () / 4);
  auto end = std::chrono::steady_clock::now ();
  return std::chrono::duration<double, std::milli> (end - start).count () / runs;
}

// Report 1080p cost per paint. Worst case is a full-frame translucent panel.
static void
report_1080p_cost (void)
{
  const size_t n = 1920 * 1080;
  std::vector<uint8_t> transparent (n * 4, 0);
  std::vector<uint8_t> translucent (n * 4);
  for (size_t i = 0; i < n; i++) {
    translucent[i * 4 + 0] = 60;
    translucent[i * 4 + 1] = 90;
    translucent[i * 4 + 2] = 120;
    translucent[i * 4 + 3] = 140;
  }
  std::vector<uint8_t> copy (n * 4);
  const int runs = 20;
  auto start = std::chrono::steady_clock::now ();
  volatile uint8_t sink = 0;
  for (int i = 0; i < runs; i++) {
    translucent[i] = (uint8_t) i;
    memcpy (copy.data (), translucent.data (), n * 4);
    sink = sink + copy[(size_t) i * 4096];
  }
  auto end = std::chrono::steady_clock::now ();
  const double memcpy_ms =
      std::chrono::duration<double, std::milli> (end - start).count () / runs;
  std::printf ("1080p per paint: memcpy %.2f ms, unpremultiply transparent %.2f ms, "
      "full translucent %.2f ms\n", memcpy_ms, time_frame_ms (transparent),
      time_frame_ms (translucent));
}

int
main (void)
{
  test_round_trip_matches_premultiplied_over ();
  test_fast_paths_and_channels ();
  test_out_of_range_clamps ();
  report_1080p_cost ();
  if (failures) {
    std::fprintf (stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf ("alpha_test: ok\n");
  return 0;
}
