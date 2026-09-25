#include "gpu_frame_queue.h"
#include <cassert>
#include <iostream>

static unsigned released = 0;
static void on_release(gpointer, GstMiniObject*) { ++released; }
static GstBuffer* frame() {
  GstBuffer* b = gst_buffer_new_allocate(nullptr, 4, nullptr);
  gst_mini_object_weak_ref(GST_MINI_OBJECT(b), on_release, nullptr);
  return b;
}
static void publish(GpuFrameQueue& q, guint64 seq) {
  GstBuffer* b = frame();
  q.Push(b, seq);
  gst_buffer_unref(b);
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstBuffer* selected = nullptr;
  guint64 seq = 0;
  {
    GpuFrameQueue q;
    publish(q, 1); q.Select(&selected, &seq); assert(seq == 1);
    publish(q, 2); publish(q, 3);
    q.Select(&selected, &seq); assert(seq == 2); // Latest-slot policy loses frame 2.
    q.Select(&selected, &seq); assert(seq == 3);
    GstBuffer* original = selected;
    q.Select(&selected, &seq); assert(seq == 3 && selected == original);
    GstBuffer* a = gst_buffer_copy(selected);
    GstBuffer* b = gst_buffer_copy(selected);
    GST_BUFFER_PTS(a) = 10; GST_BUFFER_PTS(b) = 20;
    assert(gst_buffer_peek_memory(a, 0) == gst_buffer_peek_memory(b, 0));
    assert(GST_BUFFER_PTS(selected) == GST_CLOCK_TIME_NONE);
    assert(GST_BUFFER_PTS(a) == 10 && GST_BUFFER_PTS(b) == 20);
    gst_buffer_unref(a); gst_buffer_unref(b);

    // A 30fps producer consumed at 60fps repeats each selected frame exactly twice.
    for (guint64 i = 4; i < 34; ++i) {
      publish(q, i);
      q.Select(&selected, &seq); assert(seq == i);
      q.Select(&selected, &seq); assert(seq == i);
    }
    // Fast producer retains only the newest two pending frames.
    for (guint64 i = 34; i <= 100; ++i) publish(q, i);
    q.Select(&selected, &seq); assert(seq == 99);
    q.Select(&selected, &seq); assert(seq == 100);
    q.Select(&selected, &seq); assert(seq == 100);

    // Flush drops pending output while preserving the last selected static frame.
    publish(q, 101); publish(q, 102); q.Clear();
    q.Select(&selected, &seq); assert(seq == 100);
    // Resize/popup barrier installs clean output and discards the old generation.
    publish(q, 103); publish(q, 104);
    GstBuffer* clean = frame();
    q.Reset(&selected, &seq, clean, 105);
    gst_buffer_unref(clean);
    q.Select(&selected, &seq); assert(seq == 105);
    // Downstream shared memory survives reset/stop and pending destruction.
    GstBuffer* downstream = gst_buffer_copy(selected);
    GstMemory* memory = gst_buffer_peek_memory(downstream, 0);
    q.Reset(&selected, &seq, nullptr, 0);
    assert(selected == nullptr && seq == 0);
    assert(gst_buffer_peek_memory(downstream, 0) == memory);
    gst_buffer_unref(downstream);
    // Stale UI resume tasks cannot cross another flush or stop/restart boundary.
    for (unsigned i = 0; i < 100; ++i) {
      q.BeginFlush();
      const auto old = q.ResumeGeneration();
      assert(!q.CanResume(old, true, false));
      assert(!q.CanResume(old, false, true));
      assert(q.CanResume(old, false, false));
      q.BeginFlush();
      assert(!q.CanResume(old, false, false));
      const auto latest = q.ResumeGeneration();
      q.CancelResume();
      assert(!q.CanResume(latest, false, false));
      q.BeginFlush();
      const auto restarted = q.ResumeGeneration();
      assert(q.CanResume(restarted, false, false));
      q.FinishResume();
      assert(!q.ResumePending());
    }
    publish(q, 106); publish(q, 107);
  }
  assert(released == 107);
  std::cout << "GPU frame queue cadence and lifetime tests passed\n";
}
