#include "gpu_pair_queue.h"
#include <cassert>
#include <iostream>
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GpuPairQueue queue;
  GstBuffer* a = gst_buffer_new();
  GstBuffer* b = gst_buffer_new();
  GstBuffer* c = gst_buffer_new();
  GstBuffer* d = gst_buffer_new();
  GST_BUFFER_PTS(a) = 1; GST_BUFFER_PTS(b) = 2;
  GST_BUFFER_PTS(c) = 3; GST_BUFFER_PTS(d) = 4;
  assert(queue.CanAcceptPair());
  assert(!queue.Push(a, nullptr));
  assert(!queue.Push(a, a));
  assert(queue.Pop() == nullptr);
  assert(queue.Push(a, b));
  assert(queue.Push(c, d));
  assert(!queue.CanAcceptPair());
  assert(!queue.Push(a, b));
  gst_buffer_unref(a); gst_buffer_unref(b);
  gst_buffer_unref(c); gst_buffer_unref(d);
  for (guint64 expected = 1; expected <= 4; ++expected) {
    GstBuffer* frame = queue.Pop();
    assert(frame != nullptr && GST_BUFFER_PTS(frame) == expected);
    gst_buffer_unref(frame);
    if (expected == 1) assert(!queue.CanAcceptPair());
    if (expected == 2) assert(queue.CanAcceptPair());
  }
  assert(queue.Pop() == nullptr);
  a = gst_buffer_new(); b = gst_buffer_new();
  assert(queue.Push(a, b));
  queue.Clear();
  assert(GST_MINI_OBJECT_REFCOUNT_VALUE(a) == 1);
  assert(GST_MINI_OBJECT_REFCOUNT_VALUE(b) == 1);
  assert(queue.Pop() == nullptr);
  assert(queue.CanAcceptPair());
  gst_buffer_unref(a); gst_buffer_unref(b);
  std::cout << "GPU pair queue contract passed\n";
}
