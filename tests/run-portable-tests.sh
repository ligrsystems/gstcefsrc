#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -I. tests/gpu_frame_layout_test.cc -o "$work_dir/layout-test"
"$work_dir/layout-test"
# pkg-config returns separate compiler/linker words intentionally.
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror $(pkg-config --cflags gstreamer-1.0) \
  tests/buffer_lifetime_test.cc $(pkg-config --libs gstreamer-1.0) -o "$work_dir/lifetime-test"
"$work_dir/lifetime-test"
"${CXX:-c++}" -std=c++14 -I. $(pkg-config --cflags gstreamer-audio-1.0 gstreamer-video-1.0) \
  tests/demux_lifetime_test.cc gstcefdemux.cc gstcefaudiometa.cc \
  $(pkg-config --libs gstreamer-audio-1.0 gstreamer-video-1.0) -o "$work_dir/demux-test"
"$work_dir/demux-test"
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -I. $(pkg-config --cflags gstreamer-1.0) \
  tests/gpu_pair_queue_test.cc $(pkg-config --libs gstreamer-1.0) -o "$work_dir/pair-queue-test"
"$work_dir/pair-queue-test"
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -I. tests/gpu_popup_layout_test.cc -o "$work_dir/popup-layout-test"
"$work_dir/popup-layout-test"
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -I. tests/cef_cuda_startup_test.cc -o "$work_dir/cuda-startup-test"
"$work_dir/cuda-startup-test"
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -I. $(pkg-config --cflags gstreamer-1.0) \
  tests/gpu_frame_queue_test.cc $(pkg-config --libs gstreamer-1.0) -o "$work_dir/frame-queue-test"
"$work_dir/frame-queue-test"
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror -pthread -I. $(pkg-config --cflags gstreamer-check-1.0) \
  tests/gpu_frame_clock_test.cc $(pkg-config --libs gstreamer-check-1.0) -o "$work_dir/frame-clock-test"
"$work_dir/frame-clock-test"
