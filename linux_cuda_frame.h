#pragma once
#include <memory>
#include <string>
#include <gst/cuda/gstcuda.h>
#include <include/cef_render_handler.h>

// All borrowed DMA-BUF resources are consumed and released during Copy().
// The caller serializes Copy(), Blank(), and destruction with its object lock.
class LinuxCudaFrame {
 public:
  LinuxCudaFrame(GstCudaContext* context, const GstVideoInfo& info, GstCaps* caps);
  ~LinuxCudaFrame();
  GstBuffer* Blank(std::string& error);
  GstBuffer* Copy(const CefAcceleratedPaintInfo& frame, std::string& error);
  GstBuffer* CopyPopup(const CefAcceleratedPaintInfo& frame, std::string& error);
  // A null result with an empty error means no complete view needs publication.
  GstBuffer* UpdatePopup(bool visible, const CefRect& rect, std::string& error);
 private:
  GstBuffer* CopyFrame(const CefAcceleratedPaintInfo& frame, bool popup, std::string& error);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
