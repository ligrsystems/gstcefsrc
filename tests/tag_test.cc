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
static void
draw_tag (uint8_t *row, uint32_t tag)
{
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
}

static std::vector<uint8_t>
frame_with_tag (uint32_t tag)
{
  std::vector<uint8_t> f (W * H * 4, 128);
  draw_tag (f.data () + ((H - 1) * W + (W - FRAME_TAG_PIXELS)) * 4, tag);
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

  // The squeeze tag: the claiming graphic's own tag at the bottom left, read
  // apart from the frame tag (each frame of the page paints on its own schedule).
  {
    std::vector<uint8_t> f = frame_with_tag (0x000105);
    CHECK (gst_cef_read_squeeze_tag (f.data (), W, H) == -1, "a paint without a squeeze tag reads one");
    draw_tag (f.data () + (H - 1) * W * 4, 0x000104);
    CHECK (gst_cef_read_frame_tag (f.data (), W, H) == 0x000105, "the frame tag beside a squeeze tag");
    CHECK (gst_cef_read_squeeze_tag (f.data (), W, H) == 0x000104, "the squeeze tag does not read back");
    tag_pixel (f, 0)[0] = 0;
    CHECK (gst_cef_read_squeeze_tag (f.data (), W, H) == 0x000104, "the frame tag's marker changes the squeeze tag");
    uint8_t *px = f.data () + ((H - 1) * W + 3) * 4;
    px[3] = 128;
    CHECK (gst_cef_read_squeeze_tag (f.data (), W, H) == -1, "a translucent squeeze tag pixel reads");
    std::vector<uint8_t> narrow (21 * 1 * 4, 0);
    draw_tag (narrow.data (), 0x000104);
    CHECK (gst_cef_read_squeeze_tag (narrow.data (), 21, 1) == -1, "a squeeze tag reads where it would overlap the frame tag");
  }

  // The running time of each recent tagged request, by tag: what an output
  // buffer's paint shows. Tags run in sequence and wrap at 24 bits.
  {
    GstCefTagTimes t = {};
    uint64_t time = 0;
    CHECK (!gst_cef_tag_times_get (&t, 7, &time), "a tag never requested has a time");
    const uint32_t first = FRAME_TAG_MASK - 10;
    for (uint32_t k = 0; k < 200; k++)
      gst_cef_tag_times_put (&t, (first + k) & FRAME_TAG_MASK, 1000000000ull + k * 20000000ull);
    const uint32_t newest = (first + 199) & FRAME_TAG_MASK;
    CHECK (gst_cef_tag_times_get (&t, newest, &time) && time == 1000000000ull + 199 * 20000000ull,
        "the newest tag's time");
    const uint32_t oldest_kept = (first + 200 - FRAME_TAG_TIMES) & FRAME_TAG_MASK;
    CHECK (gst_cef_tag_times_get (&t, oldest_kept, &time) && time == 1000000000ull + (200 - FRAME_TAG_TIMES) * 20000000ull,
        "the oldest kept tag's time");
    CHECK (!gst_cef_tag_times_get (&t, (oldest_kept - 1) & FRAME_TAG_MASK, &time), "a tag older than the ring has a time");
    CHECK (!gst_cef_tag_times_get (&t, (newest + 1) & FRAME_TAG_MASK, &time), "a tag not yet requested has a time");
  }

  if (failures) {
    std::fprintf (stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf ("tag_test: all checks passed\n");
  return 0;
}
