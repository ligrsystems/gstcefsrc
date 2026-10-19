#ifndef __GST_CEF_TAG_H__
#define __GST_CEF_TAG_H__

#include <stddef.h>
#include <stdint.h>

/* The frame tag. The page's AR layer draws it at the bottom right of every
 * paint (ligr packages/shared/src/ar/tag.ts); frame-tag mode outputs the
 * paint whose tag matches the video frame being composited.
 *
 * FRAME_TAG_PIXELS opaque pixels: a magenta marker, then 8 pixels of 3 bits
 * each (R = bit 2, G = bit 1, B = bit 0, least significant group first: the
 * 24-bit tag), then 2 pixels with the tag's 6-bit check, the same way.
 *
 * A graphic over the tag, opaque or translucent, must make the read fail,
 * never give another tag: every channel must be plainly on or off, every
 * pixel opaque, and the check must match. */
#define FRAME_TAG_PIXELS 11
#define FRAME_TAG_MASK 0xffffff

/* CRC-6 (x^6 + x + 1) of the 24-bit tag, most significant bit first, XOR
 * 0x2a. The CRC catches every change inside one pixel's 3 bits; the XOR
 * keeps any one colour over the data and check pixels from reading as a tag
 * (a plain CRC reads all black as tag 0). */
static inline uint32_t
gst_cef_frame_tag_check (uint32_t tag)
{
  uint32_t crc = 0;
  for (int i = 23; i >= 0; i--) {
    uint32_t bit = ((tag >> i) & 1u) ^ ((crc >> 5) & 1u);
    crc = (crc << 1) & 0x3fu;
    if (bit)
      crc ^= 0x03u;
  }
  return crc ^ 0x2au;
}

/* The 3 bits of one BGRA tag pixel, or -1 when it is not opaque or a channel
 * is neither plainly on nor off (a blend). */
static inline int
gst_cef_frame_tag_bits (const uint8_t *px)
{
  if (px[3] < 200)
    return -1;
  int bits = 0;
  for (int c = 2; c >= 0; c--) {
    if (px[c] >= 64 && px[c] < 192)
      return -1;
    bits = (bits << 1) | (px[c] >= 192);
  }
  return bits;
}

/* The tag at the bottom right of a BGRA frame `w` x `h`, or -1 when it is
 * absent, covered or blended. */
static inline int64_t
gst_cef_read_frame_tag (const uint8_t *bgra, int w, int h)
{
  if (w < FRAME_TAG_PIXELS || h < 1)
    return -1;
  const uint8_t *row = bgra + ((size_t) (h - 1) * w + (w - FRAME_TAG_PIXELS)) * 4;
  if (row[0] < 200 || row[1] > 55 || row[2] < 200 || row[3] < 200)
    return -1;
  uint32_t tag = 0, check = 0;
  for (int j = 0; j < 10; j++) {
    int bits = gst_cef_frame_tag_bits (row + (1 + j) * 4);
    if (bits < 0)
      return -1;
    if (j < 8)
      tag |= (uint32_t) bits << (3 * j);
    else
      check |= (uint32_t) bits << (3 * (j - 8));
  }
  return check == gst_cef_frame_tag_check (tag) ? (int64_t) tag : -1;
}

/* The running time each recent tagged request asked for, by tag. Frame-tag
 * mode stamps an output buffer with the running time of the frame its paint
 * shows: the request that carried the paint's tag. Requests come one per
 * video frame with tags in sequence, so FRAME_TAG_TIMES recent ones (about
 * 1.3 s at 50 fps) cover any paint the output can repeat. */
#define FRAME_TAG_TIMES 64

typedef struct {
  uint32_t tag[FRAME_TAG_TIMES];
  uint64_t time[FRAME_TAG_TIMES];
  uint8_t set[FRAME_TAG_TIMES];
} GstCefTagTimes;

static inline void
gst_cef_tag_times_put (GstCefTagTimes *t, uint32_t tag, uint64_t time)
{
  size_t i = tag % FRAME_TAG_TIMES;
  t->tag[i] = tag;
  t->time[i] = time;
  t->set[i] = 1;
}

/* 1 and the running time requested with `tag`, or 0 when it is too old or
 * was never requested. */
static inline int
gst_cef_tag_times_get (const GstCefTagTimes *t, uint32_t tag, uint64_t *time)
{
  size_t i = tag % FRAME_TAG_TIMES;
  if (!t->set[i] || t->tag[i] != tag)
    return 0;
  *time = t->time[i];
  return 1;
}

#endif /* __GST_CEF_TAG_H__ */
