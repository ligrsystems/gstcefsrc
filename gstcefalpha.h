#ifndef __GST_CEF_ALPHA_H__
#define __GST_CEF_ALPHA_H__

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define GST_CEF_ALPHA_SSE2 1
#endif

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

static inline void
gst_cef_unpremultiply_bgra_scalar (uint8_t * dst, const uint8_t * src,
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

/* Copy n_pixels BGRA pixels from src to dst, converting premultiplied alpha
 * to straight alpha. Groups of opaque or fully transparent pixels take a
 * fast path. Results match the scalar table to the last bit. */
static inline void
gst_cef_unpremultiply_bgra (uint8_t * dst, const uint8_t * src,
    size_t n_pixels)
{
#ifdef GST_CEF_ALPHA_SSE2
  const __m128i alpha_mask = _mm_set1_epi32 ((int) 0xFF000000u);
  const __m128i low_byte = _mm_set1_epi32 (0xFF);
  const __m128i max_value = _mm_set1_epi32 (255);
  const __m128i half = _mm_set1_epi32 (32768);
  const uint32_t *table = gst_cef_unpremultiply_table ();
  size_t i = 0;

  for (; i + 4 <= n_pixels; i += 4, src += 16, dst += 16) {
    __m128i px = _mm_loadu_si128 ((const __m128i *) src);
    const __m128i alpha = _mm_and_si128 (px, alpha_mask);
    const int opaque = _mm_movemask_epi8 (_mm_cmpeq_epi32 (alpha, alpha_mask));
    const int clear = _mm_movemask_epi8 (_mm_cmpeq_epi32 (alpha, _mm_setzero_si128 ()));

    if ((opaque | clear) != 0xFFFF) {
      /* Per-lane reciprocal from the table; SSE2 has no gather. */
      const __m128i r = _mm_set_epi32 ((int) table[src[15]], (int) table[src[11]],
          (int) table[src[7]], (int) table[src[3]]);
      __m128i out = alpha;
      for (int shift = 0; shift < 24; shift += 8) {
        const __m128i c = _mm_and_si128 (_mm_srli_epi32 (px, shift), low_byte);
        /* c * r < 2^24, so the even/odd 32-bit multiplies cannot overflow. */
        const __m128i even = _mm_mul_epu32 (c, r);
        const __m128i odd = _mm_mul_epu32 (_mm_srli_epi64 (c, 32), _mm_srli_epi64 (r, 32));
        __m128i v = _mm_or_si128 (_mm_and_si128 (even, _mm_set_epi32 (0, -1, 0, -1)),
            _mm_slli_epi64 (odd, 32));
        v = _mm_srli_epi32 (_mm_add_epi32 (v, half), 16);
        const __m128i over = _mm_cmpgt_epi32 (v, max_value);
        v = _mm_or_si128 (_mm_andnot_si128 (over, v), _mm_and_si128 (over, max_value));
        out = _mm_or_si128 (out, _mm_slli_epi32 (v, shift));
      }
      px = out;
    } else if (clear == 0xFFFF) {
      px = _mm_setzero_si128 ();
    } else if (opaque != 0xFFFF) {
      /* Only opaque and clear pixels: keep the opaque ones. */
      px = _mm_andnot_si128 (_mm_cmpeq_epi32 (alpha, _mm_setzero_si128 ()), px);
    }
    _mm_storeu_si128 ((__m128i *) dst, px);
  }
  gst_cef_unpremultiply_bgra_scalar (dst, src, n_pixels - i);
#else
  gst_cef_unpremultiply_bgra_scalar (dst, src, n_pixels);
#endif
}

#endif /* __GST_CEF_ALPHA_H__ */
