#pragma once
#include <gst/gst.h>
#include <deque>

// The caller holds the source object lock. Partial pairs never enter the queue.
class GpuPairQueue {
 public:
  GpuPairQueue() = default;
  GpuPairQueue(const GpuPairQueue&) = delete;
  GpuPairQueue& operator=(const GpuPairQueue&) = delete;
  ~GpuPairQueue() { Clear(); }
  bool CanAcceptPair() const { return buffers_.size() <= 2; }
  bool Push(GstBuffer* first, GstBuffer* second) {
    if (!first || !second || first == second || !CanAcceptPair()) return false;
    buffers_.push_back(gst_buffer_ref(first));
    buffers_.push_back(gst_buffer_ref(second));
    return true;
  }
  GstBuffer* Pop() {
    if (buffers_.empty()) return nullptr;
    GstBuffer* result = buffers_.front();
    buffers_.pop_front();
    return result;
  }
  void Clear() {
    for (GstBuffer* buffer : buffers_) gst_buffer_unref(buffer);
    buffers_.clear();
  }
 private:
  std::deque<GstBuffer*> buffers_;
};
