#include "cef_cuda_startup.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>

struct CommandLine {
  std::map<std::string, std::string> switches;
  bool HasSwitch(const char* name) const { return switches.count(name) != 0; }
  void AppendSwitchWithValue(const char* name, const char* value) {
    switches[name] = value;
  }
};

int main() {
  CommandLine cpu;
  gst_cef_apply_cuda_startup_defaults(false, cpu);
  assert(cpu.switches.empty());

  CommandLine cuda;
  gst_cef_apply_cuda_startup_defaults(true, cuda);
  assert(cuda.switches["use-angle"] == "gl-egl");
  assert(cuda.switches["ozone-platform"] == "x11");

  CommandLine explicit_flags;
  explicit_flags.switches = {{"use-angle", "vulkan"}, {"ozone-platform", "wayland"}};
  gst_cef_apply_cuda_startup_defaults(true, explicit_flags);
  assert(explicit_flags.switches["use-angle"] == "vulkan");
  assert(explicit_flags.switches["ozone-platform"] == "wayland");

  CommandLine partial;
  partial.switches["use-angle"] = "";
  gst_cef_apply_cuda_startup_defaults(true, partial);
  assert(partial.switches["use-angle"].empty());
  assert(partial.switches["ozone-platform"] == "x11");
  std::cout << "CUDA startup defaults and explicit override tests passed\n";
}
