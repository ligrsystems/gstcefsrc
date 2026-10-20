// Portable checks for the frame tag reader. No CEF or GStreamer needed:
//   c++ -std=c++14 -O2 -Wall -Wextra -Werror -I. tests/tag_test.cc -o tag-test
#include "gstceftag.h"

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

static const int W = 64, H = 4;

// The page's encoder (ligr packages/shared/src/ar/tag.ts, encodeTag), as BGRA
// at the bottom right of a W x H frame over a mid-grey picture.
static std::vector<uint8_t>
frame_with_tag (uint32_t tag)
{
  std::vector<uint8_t> f (W * H * 4, 128);
  uint8_t *row = f.data () + ((H - 1) * W + (W - FRAME_TAG_PIXELS)) * 4;
  const uint8_t marker[4] = { 255, 0, 255, 255 };
  for (int c = 0; c < 4; c++)
    row[c] = marker[c];
  uint32_t check = gst_cef_frame_tag_check (tag);
  for (int j = 0; j < 10; j++) {
    uint32_t bits = j < 8 ? (tag >> (3 * j)) & 7u : (check >> (3 * (j - 8))) & 7u;
    uint8_t *px = row + (1 + j) * 4;
    px[2] = bits & 4 ? 255 : 0;  // R
    px[1] = bits & 2 ? 255 : 0;  // G
    px[0] = bits & 1 ? 255 : 0;  // B
    px[3] = 255;
  }
  return f;
}

static uint8_t *
tag_pixel (std::vector<uint8_t> &f, int k)
{
  return f.data () + ((H - 1) * W + (W - FRAME_TAG_PIXELS) + k) * 4;
}

int
main ()
{
  // The check values ligr's tests hold too (ar.test.ts), so both sides agree.
  const uint32_t vectors[][2] = {
    { 0x000000, 0x2a }, { 0x000001, 0x29 }, { 0x123456, 0x39 },
    { 0xabcdef, 0x39 }, { 0xffffff, 0x3a }, { 0x001092, 0x19 },
  };
  for (const auto &v : vectors)
    CHECK (gst_cef_frame_tag_check (v[0]) == v[1], "check of %06x is %02x, want %02x", v[0],
        gst_cef_frame_tag_check (v[0]), v[1]);

  // Every tag reads back.
  const uint32_t edges[] = { 0, 1, 0x123456, FRAME_TAG_MASK };
  uint32_t seed = 12345;
  for (int i = 0; i < 2000; i++) {
    seed = seed * 1103515245u + 12345u;
    uint32_t tag = i < 4 ? edges[i] : (seed >> 4) & FRAME_TAG_MASK;
    std::vector<uint8_t> f = frame_with_tag (tag);
    CHECK (gst_cef_read_frame_tag (f.data (), W, H) == (int64_t) tag, "tag %06x does not read back", tag);

    // Any other plain colour in any one data or check pixel fails the check.
    if (i < 200) {
      for (int k = 1; k < FRAME_TAG_PIXELS; k++) {
        uint8_t *px = tag_pixel (f, k);
        const uint8_t saved[3] = { px[0], px[1], px[2] };
        for (int bits = 0; bits < 8; bits++) {
          px[2] = bits & 4 ? 255 : 0;
          px[1] = bits & 2 ? 255 : 0;
          px[0] = bits & 1 ? 255 : 0;
          bool same = px[0] == saved[0] && px[1] == saved[1] && px[2] == saved[2];
          int64_t got = gst_cef_read_frame_tag (f.data (), W, H);
          CHECK (same ? got == (int64_t) tag : got == -1, "tag %06x pixel %d bits %d read %lld", tag, k, bits,
              (long long) got);
        }
        px[0] = saved[0];
        px[1] = saved[1];
        px[2] = saved[2];
      }
    }
  }

  // An opaque graphic over the data and check pixels, the magenta marker
  // left in view: no single colour reads as a tag (a plain CRC would read
  // all black as tag 0).
  for (int bits = 0; bits < 8; bits++) {
    std::vector<uint8_t> f = frame_with_tag (0x123456);
    for (int k = 1; k < FRAME_TAG_PIXELS; k++) {
      uint8_t *px = tag_pixel (f, k);
      px[2] = bits & 4 ? 255 : 0;
      px[1] = bits & 2 ? 255 : 0;
      px[0] = bits & 1 ? 255 : 0;
      px[3] = 255;
    }
    CHECK (gst_cef_read_frame_tag (f.data (), W, H) == -1, "a cover of colour %d reads as a tag", bits);
  }

  // An opaque graphic over the whole tag, in any colour: no marker, no tag.
  const uint8_t covers[][4] = { { 0, 0, 0, 255 }, { 255, 255, 255, 255 }, { 40, 30, 200, 255 }, { 128, 128, 128, 255 } };
  for (const auto &c : covers) {
    std::vector<uint8_t> f = frame_with_tag (0x00beef);
    for (int k = 0; k < FRAME_TAG_PIXELS; k++)
      for (int ch = 0; ch < 4; ch++)
        tag_pixel (f, k)[ch] = c[ch];
    CHECK (gst_cef_read_frame_tag (f.data (), W, H) == -1, "a full cover reads as a tag");
  }

  // A translucent graphic over the tag: the blended channels are rejected,
  // even where the marker stays magenta enough (a 30 % white wash).
  for (double a : { 0.3, 0.5, 0.7 }) {
    for (int white = 0; white <= 1; white++) {
      std::vector<uint8_t> f = frame_with_tag (0x5a5a5a);
      for (int k = 0; k < FRAME_TAG_PIXELS; k++)
        for (int ch = 0; ch < 3; ch++) {
          uint8_t *px = tag_pixel (f, k);
          px[ch] = (uint8_t) (px[ch] * (1.0 - a) + (white ? 255 : 0) * a + 0.5);
        }
      CHECK (gst_cef_read_frame_tag (f.data (), W, H) == -1, "a %.0f %% %s wash reads as a tag", a * 100,
          white ? "white" : "black");
    }
  }

  // A pixel that is not opaque is no tag pixel.
  {
    std::vector<uint8_t> f = frame_with_tag (0x000777);
    tag_pixel (f, 5)[3] = 120;
    CHECK (gst_cef_read_frame_tag (f.data (), W, H) == -1, "a translucent tag pixel reads");
  }

  // Too narrow a frame has no tag.
  {
    std::vector<uint8_t> f (8 * 1 * 4, 255);
    CHECK (gst_cef_read_frame_tag (f.data (), 8, 1) == -1, "an 8 px frame reads a tag");
  }

  if (failures) {
    std::fprintf (stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf ("tag_test: all checks passed\n");
  return 0;
}
