#pragma once
#include <gst/gst.h>
#include <array>

// The caller holds the source object lock. Entries own references to completed
// frames. The last selected buffer belongs to the caller, outside this queue.
class GpuFrameQueue {
 public:
  GpuFrameQueue() = default;
  GpuFrameQueue(const GpuFrameQueue&) = delete;
  GpuFrameQueue& operator=(const GpuFrameQueue&) = delete;
  ~GpuFrameQueue() { Clear(); }

  guint64 Push(GstBuffer* buffer, guint64 sequence) {
    guint64 dropped = 0;
    if (size_ == pending_.size()) {
      dropped = pending_[0].sequence;
      RemoveFirst();
    }
    pending_[size_++] = {gst_buffer_ref(buffer), sequence};
    return dropped;
  }

  void Select(GstBuffer** current, guint64* sequence) {
    if (!size_) return;
    gst_buffer_replace(current, pending_[0].buffer);
    *sequence = pending_[0].sequence;
    RemoveFirst();
  }

  void Clear() { while (size_) RemoveFirst(); }

  // Popup changes and resize must not replay frames from the preceding state.
  void Reset(GstBuffer** current, guint64* sequence, GstBuffer* replacement, guint64 next_sequence) {
    Clear();
    gst_buffer_replace(current, replacement);
    *sequence = next_sequence;
  }

  // Resume tasks carry a generation so another flush/stop invalidates them.
  void BeginFlush() { Clear(); ++resume_generation_; resume_pending_ = true; }
  guint64 ResumeGeneration() const { return resume_generation_; }
  bool ResumePending() const { return resume_pending_; }
  bool CanResume(guint64 generation, bool flushing, bool stopping) const {
    return resume_pending_ && generation == resume_generation_ && !flushing && !stopping;
  }
  void FinishResume() { resume_pending_ = false; }
  void CancelResume() { ++resume_generation_; resume_pending_ = false; }

 private:
  guint64 resume_generation_ = 0;
  bool resume_pending_ = false;
  struct Frame { GstBuffer* buffer = nullptr; guint64 sequence = 0; };
  std::array<Frame, 2> pending_{};
  size_t size_ = 0;

  void RemoveFirst() {
    gst_buffer_unref(pending_[0].buffer);
    if (size_ == 2) pending_[0] = pending_[1];
    pending_[--size_] = {};
  }
};
