#include "gpu_frame_clock.h"
#include <gst/check/gsttestclock.h>
#include <cassert>
#include <future>
#include <iostream>

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstClock* clock = gst_test_clock_new_with_start_time(1050 * GST_MSECOND);
  GpuFrameClock scheduler;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  assert(scheduler.Wait(clock, GST_SECOND, 60, 1, &pts) == GpuFrameClock::Result::Ready);
  assert(pts == 50 * GST_MSECOND); // Use pipeline running time, not a zero-based frame count.
  auto pending = [&]() {
    GstClockID id = nullptr;
    gst_test_clock_wait_for_next_pending_id(GST_TEST_CLOCK(clock), &id);
    return id;
  };
  auto next = [&]() { return scheduler.Wait(clock, GST_SECOND, 60, 1, &pts); };
  auto wait = std::async(std::launch::async, next);
  GstClockID id = pending();
  assert(gst_clock_id_get_time(id) == GST_SECOND + 50 * GST_MSECOND + GST_SECOND / 60);
  assert(wait.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  gst_test_clock_set_time(GST_TEST_CLOCK(clock), gst_clock_id_get_time(id));
  gst_test_clock_process_id(GST_TEST_CLOCK(clock), id);
  assert(wait.get() == GpuFrameClock::Result::Ready);
  assert(pts == 50 * GST_MSECOND + GST_SECOND / 60);

  // A late wake uses the current due slot, not the deadline from before the stall.
  wait = std::async(std::launch::async, next);
  id = pending();
  gst_test_clock_set_time(GST_TEST_CLOCK(clock), 1500 * GST_MSECOND);
  gst_test_clock_process_id(GST_TEST_CLOCK(clock), id);
  assert(wait.get() == GpuFrameClock::Result::Ready);
  assert(pts == 500 * GST_MSECOND);

  // Flush cancels a real blocked clock wait and rejects work until resume.
  wait = std::async(std::launch::async, next);
  id = pending();
  scheduler.Cancel();
  assert(wait.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  assert(wait.get() == GpuFrameClock::Result::Cancelled);
  gst_clock_id_unref(id);
  assert(next() == GpuFrameClock::Result::Cancelled);
  scheduler.Resume();
  gst_test_clock_set_time(GST_TEST_CLOCK(clock), 2 * GST_SECOND);
  assert(next() == GpuFrameClock::Result::Ready && pts == GST_SECOND);

  // Clock changes retry instead of reporting a flush, then anchor to the new clock.
  wait = std::async(std::launch::async, next);
  id = pending();
  scheduler.Invalidate();
  assert(wait.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  assert(wait.get() == GpuFrameClock::Result::Retry);
  gst_clock_id_unref(id);
  GstClock* replacement = gst_test_clock_new_with_start_time(9 * GST_SECOND);
  assert(scheduler.Wait(replacement, 8 * GST_SECOND, 60, 1, &pts) == GpuFrameClock::Result::Ready);
  assert(pts == GST_SECOND);
  bool discont = false;
  assert(scheduler.Wait(replacement, 7 * GST_SECOND, 60, 1, &pts, &discont) == GpuFrameClock::Result::Ready);
  assert(pts == 2 * GST_SECOND && discont);

  // A stall skips elapsed schedule slots; it does not produce a catch-up burst.
  gst_test_clock_set_time(GST_TEST_CLOCK(replacement), 10 * GST_SECOND);
  assert(scheduler.Wait(replacement, 7 * GST_SECOND, 60, 1, &pts, &discont) == GpuFrameClock::Result::Ready);
  assert(pts == 3 * GST_SECOND && discont);
  scheduler.Reset();
  // Fractional rates use a fixed rational origin, without accumulating rounding error.
  assert(scheduler.Wait(replacement, 7 * GST_SECOND, 30000, 1001, &pts) == GpuFrameClock::Result::Ready);
  for (guint64 i = 1; i <= 1000; ++i) {
    const GstClockTime expected = 3 * GST_SECOND + gst_util_uint64_scale(i, 1001 * GST_SECOND, 30000);
    gst_test_clock_set_time(GST_TEST_CLOCK(replacement), 7 * GST_SECOND + expected);
    assert(scheduler.Wait(replacement, 7 * GST_SECOND, 30000, 1001, &pts) == GpuFrameClock::Result::Ready);
    assert(pts == expected);
  }
  gst_object_unref(replacement);
  gst_object_unref(clock);
  std::cout << "GPU frame clock tests passed\n";
}
