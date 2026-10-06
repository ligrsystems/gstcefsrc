#include "gpu_frame_layout.h"
#include <cassert>
#include <climits>
#include <iostream>
using namespace cef_gpu;
int main() {
  Frame f{1920,1088,0,0,1920,1080,1,4,7680,0,8355840,0,Format::BGRA};
  Layout l{};
  assert(validate(f,1920,1080,&l)==nullptr);
  assert(l.drm_fourcc==fourcc('A','R','2','4'));
  f.format=Format::RGBA;
  assert(validate(f,1920,1080,&l)==nullptr);
  assert(l.drm_fourcc==fourcc('A','B','2','4'));
  f.x=1; assert(validate(f,1920,1080,&l)!=nullptr); f.x=0;
  f.y=8; assert(validate(f,1920,1080,&l)==nullptr);
  f.y=9; assert(validate(f,1920,1080,&l)!=nullptr); f.y=0;
  f.stride=7679; assert(validate(f,1920,1080,&l)!=nullptr); f.stride=7680;
  f.plane_count=2; assert(validate(f,1920,1080,&l)!=nullptr); f.plane_count=1;
  f.fd=-1; assert(validate(f,1920,1080,&l)!=nullptr); f.fd=4;
  f.offset=uint64_t(INT_MAX)+1; assert(validate(f,1920,1080,&l)!=nullptr); f.offset=0;
  f.size=100; assert(validate(f,1920,1080,&l)!=nullptr); f.size=8355840;
  f.modifier=0x0300000000000010ULL;
  assert(validate(f,1920,1080,&l)==nullptr); // Tiled size is opaque; EGL validates it.
  f.size=100; assert(validate(f,1920,1080,&l)==nullptr);
  f.format=Format::Unsupported; assert(validate(f,1920,1080,&l)!=nullptr);
  f.format=Format::RGBA;
  assert(validate(f,1280,720,&l)!=nullptr); // Never crop a stale resize into new caps.
  f.width=0; assert(validate(f,1920,1080,&l)!=nullptr);
  std::cout << "GPU frame layout tests passed\n";
}
