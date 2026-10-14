#ifndef __GST_CEF_ALPHA_H__
#define __GST_CEF_ALPHA_H__

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Chromium paints premultiplied BGRA. GStreamer BGRA caps mean straight
 * alpha, and compositor / cudacompositor blend as src * a + dst * (1 - a).
 * Without this conversion every translucent pixel is multiplied by its
 * alpha twice and comes out darker. */

/* round (255 * 65536 / a), so c * 255 / a == (c * table[a] + 32768) >> 16. */
struct GstCefUnpremultiplyTable
{
  uint32_t v[256];
  GstCefUnpremultiplyTable ()
  {
    v[0] = 0;
    for (uint32_t a = 1; a < 256; a++)
      v[a] = (255u * 65536u + a / 2) / a;
  }
};

static inline const uint32_t *
gst_cef_unpremultiply_table (void)
{
  /* C++11 guarantees thread-safe initialization of function-local statics. */
  static const GstCefUnpremultiplyTable table;
  return table.v;
}

/* Copy n_pixels BGRA pixels from src to dst, converting premultiplied alpha
 * to straight alpha. Opaque and fully transparent pixels take a fast path. */
static inline void
gst_cef_unpremultiply_bgra (uint8_t * dst, const uint8_t * src,
    size_t n_pixels)
{
  const uint32_t *table = gst_cef_unpremultiply_table ();

  for (size_t i = 0; i < n_pixels; i++, src += 4, dst += 4) {
    uint32_t px;
    memcpy (&px, src, 4);
    const uint32_t a = src[3];
    if (a != 255 && a != 0) {
      const uint32_t r = table[a];
      uint8_t out[4];
      for (int c = 0; c < 3; c++) {
        const uint32_t v = (src[c] * r + 32768) >> 16;
        out[c] = (uint8_t) (v > 255 ? 255 : v);
      }
      out[3] = (uint8_t) a;
      memcpy (&px, out, 4);
    } else if (a == 0) {
      px = 0;
    }
    memcpy (dst, &px, 4);
  }
}

#endif /* __GST_CEF_ALPHA_H__ */
