#include <gst/gst.h>
#include <cassert>
#include <iostream>

// This dependency-contract test uses the same shallow-copy operation as create().
// The negative control demonstrates why a shared buffer header is invalid.
static GstBuffer* output_frame(GstBuffer* current) {
#ifdef GPU_TEST_REFERENCE_HEADER
  return gst_buffer_ref(current);
#else
  return gst_buffer_copy(current);
#endif
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstBufferPool* pool = gst_buffer_pool_new();
  GstStructure* config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, nullptr, 16, 0, 2);
  assert(gst_buffer_pool_set_config(pool, config));
  assert(gst_buffer_pool_set_active(pool, TRUE));
  GstBuffer* current = nullptr;
  assert(gst_buffer_pool_acquire_buffer(pool, &current, nullptr) == GST_FLOW_OK);
  gst_buffer_memset(current, 0, 0x5a, 16);
  GstBuffer* earlier = output_frame(current);
  GST_BUFFER_PTS(earlier) = GST_SECOND;
  GstBuffer* later = output_frame(current);
  GST_BUFFER_PTS(later) = 2 * GST_SECOND;
  assert(GST_BUFFER_PTS(earlier) == GST_SECOND);
  assert(GST_BUFFER_PTS(current) == GST_CLOCK_TIME_NONE);
  GstParentBufferMeta* parent = gst_buffer_get_parent_buffer_meta(earlier);
  guint major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  // Require automatic parent retention on 1.24+; some older releases lack it.
  // Older pools discard shared-memory buffers instead of reusing their memory.
  if (major > 1 || (major == 1 && minor >= 24)) assert(parent != nullptr);
  if (parent) assert(parent->buffer == current);
  GstMemory* held_memory = gst_buffer_peek_memory(earlier, 0);
  gst_buffer_unref(current);
  gst_buffer_unref(later);
  GstBuffer* next = nullptr;
  assert(gst_buffer_pool_acquire_buffer(pool, &next, nullptr) == GST_FLOW_OK);
  assert(gst_buffer_peek_memory(next, 0) != held_memory);
  gst_buffer_memset(next, 0, 0xff, 16);
  unsigned char pixels[16];
  assert(gst_buffer_extract(earlier, 0, pixels, sizeof(pixels)) == sizeof(pixels));
  for (auto value : pixels) assert(value == 0x5a);
  gst_buffer_unref(earlier);
  gst_buffer_unref(next);
  assert(gst_buffer_pool_set_active(pool, FALSE));
  gst_object_unref(pool);
  std::cout << "GStreamer buffer lifetime contract passed\n";
}
