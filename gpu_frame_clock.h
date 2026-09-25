#pragma once
#include <gst/gst.h>
#include <mutex>

// One streaming thread calls Wait. Lifecycle threads can cancel or invalidate it.
// No element lock is acquired here, and the mutex is released during clock waits.
class GpuFrameClock {
 public:
  enum class Result { Ready, Cancelled, Retry, Error };
  ~GpuFrameClock() { Cancel(); gst_clear_object(&clock_); }
  Result Wait(GstClock* clock, GstClockTime base, int fps_n, int fps_d,
              GstClockTime* pts, bool* discont = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cancelled_) return Result::Cancelled;
    if (!clock || !GST_CLOCK_TIME_IS_VALID(base) || fps_n <= 0 || fps_d <= 0)
      return Result::Error;
    const GstClockTime now = gst_clock_get_time(clock);
    if (now < base) return Result::Error;
    const GstClockTime running = now - base;
    if (clock_ != clock || base_ != base || fps_n_ != fps_n || fps_d_ != fps_d) {
      gst_object_replace(reinterpret_cast<GstObject**>(&clock_), GST_OBJECT(clock));
      base_ = base; fps_n_ = fps_n; fps_d_ = fps_d;
      anchor_ = GST_CLOCK_TIME_NONE;
    }
    if (!GST_CLOCK_TIME_IS_VALID(anchor_)) {
      anchor_ = running;
      slot_ = 0;
      discontinuity_ = true;
    }
    const guint64 scale = GST_SECOND * static_cast<guint64>(fps_d);
    const guint64 due = running > anchor_ ? gst_util_uint64_scale(running - anchor_, fps_n, scale) : 0;
    if (due > slot_) { slot_ = due; discontinuity_ = true; }
    GstClockTime target = anchor_ + gst_util_uint64_scale(slot_, scale, fps_n);
    const guint64 generation = generation_;
    if (target > running) {
      GstClockID id = gst_clock_new_single_shot_id(clock, base + target);
      wait_id_ = id;
      lock.unlock();
      const GstClockReturn result = gst_clock_id_wait(id, nullptr);
      lock.lock();
      wait_id_ = nullptr;
      gst_clock_id_unref(id);
      if (cancelled_) return Result::Cancelled;
      if (generation != generation_) return Result::Retry;
      if (result != GST_CLOCK_OK && result != GST_CLOCK_EARLY) return Result::Error;
      const GstClockTime woke = gst_clock_get_time(clock);
      const guint64 woke_slot = woke > base + anchor_ ?
          gst_util_uint64_scale(woke - base - anchor_, fps_n, scale) : 0;
      if (woke_slot > slot_) {
        slot_ = woke_slot;
        target = anchor_ + gst_util_uint64_scale(slot_, scale, fps_n);
        discontinuity_ = true;
      }
    }
    *pts = target;
    if (discont) *discont = discontinuity_;
    discontinuity_ = false;
    ++slot_;
    return Result::Ready;
  }
  void Cancel() { std::lock_guard<std::mutex> lock(mutex_); cancelled_ = true; InvalidateLocked(); }
  void Resume() { std::lock_guard<std::mutex> lock(mutex_); cancelled_ = false; InvalidateLocked(); }
  void Reset() { Resume(); }
  void Invalidate() { std::lock_guard<std::mutex> lock(mutex_); InvalidateLocked(); }

 private:
  void InvalidateLocked() {
    ++generation_;
    anchor_ = GST_CLOCK_TIME_NONE;
    if (wait_id_) gst_clock_id_unschedule(wait_id_);
  }
  std::mutex mutex_;
  GstClock* clock_ = nullptr;
  GstClockID wait_id_ = nullptr;
  GstClockTime base_ = GST_CLOCK_TIME_NONE;
  GstClockTime anchor_ = GST_CLOCK_TIME_NONE;
  guint64 slot_ = 0, generation_ = 0;
  int fps_n_ = 0, fps_d_ = 0;
  bool cancelled_ = false, discontinuity_ = true;
};
