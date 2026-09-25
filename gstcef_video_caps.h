#pragma once
#define CEF_SYSTEM_VIDEO_CAPS "video/x-raw, format=BGRA, width=[1, 2147483647], height=[1, 2147483647], framerate=[1/1, 60/1], pixel-aspect-ratio=1/1"
#ifdef GST_CEF_ENABLE_CUDA
#define CEF_CUDA_VIDEO_CAPS "video/x-raw(memory:CUDAMemory), format=BGRA, width=[1, 2147483647], height=[1, 2147483647], framerate=[1/1, 60/1], pixel-aspect-ratio=1/1"
#define CEF_VIDEO_CAPS CEF_SYSTEM_VIDEO_CAPS "; " CEF_CUDA_VIDEO_CAPS
#else
#define CEF_VIDEO_CAPS CEF_SYSTEM_VIDEO_CAPS
#endif
