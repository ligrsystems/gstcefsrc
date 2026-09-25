#ifndef GST_CEF_CUDA_STARTUP_H
#define GST_CEF_CUDA_STARTUP_H

template <typename CommandLine>
void gst_cef_apply_cuda_startup_defaults(bool cuda_memory, CommandLine& command_line) {
  if (!cuda_memory) return;
  if (!command_line.HasSwitch("use-angle")) {
    command_line.AppendSwitchWithValue("use-angle", "gl-egl");
  }
  if (!command_line.HasSwitch("ozone-platform")) {
    command_line.AppendSwitchWithValue("ozone-platform", "x11");
  }
}

#endif
