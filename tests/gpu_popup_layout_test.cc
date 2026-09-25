#include "gpu_popup_layout.h"
#include <cassert>
#include <climits>
#include <iostream>
using namespace cef_gpu;
int main() {
  auto p = clip_popup(1920,1080,{20,30,300,200});
  assert(p.x==20 && p.y==30 && p.width==300 && p.height==200 && p.source_x==0 && p.source_y==0);
  p=clip_popup(1920,1080,{-10,-20,300,200});
  assert(p.x==0 && p.y==0 && p.width==290 && p.height==180 && p.source_x==10 && p.source_y==20);
  p=clip_popup(1920,1080,{1900,1060,300,200});
  assert(p.x==1900 && p.y==1060 && p.width==20 && p.height==20 && p.source_x==0 && p.source_y==0);
  p=clip_popup(1920,1080,{-10,-20,3000,2000});
  assert(p.width==1920 && p.height==1080 && p.source_x==10 && p.source_y==20);
  assert(clip_popup(1920,1080,{1920,0,100,100}).width==0);
  assert(clip_popup(1920,1080,{-100,0,100,100}).width==0);
  assert(clip_popup(1920,1080,{INT_MAX,INT_MAX,INT_MAX,INT_MAX}).width==0);
  assert(clip_popup(1920,1080,{INT_MIN,0,INT_MAX,100}).width==0);
  assert(clip_popup(0,1080,{0,0,10,10}).width==0);
  assert(clip_popup(1920,1080,{0,0,0,10}).width==0);
  assert(same_rect({1,2,3,4},{1,2,3,4}));
  assert(!same_rect({1,2,3,4},{1,2,3,5}));
  PopupState state;
  auto change = state.Update(true, {20,30,300,200});
  assert(state.visible && change.clear_pixels && change.publish);
  state.has_pixels = true; // An owned popup copy completed.
  change = state.Update(true, {20,30,300,200});
  assert(!change.clear_pixels && !change.publish && state.has_pixels);
  change = state.Update(true, {50,60,300,200});
  assert(!change.clear_pixels && change.publish && state.has_pixels);
  change = state.Update(true, {50,60,200,100});
  assert(change.clear_pixels && change.publish && !state.has_pixels);
  state.has_pixels = true;
  change = state.Update(false, {});
  assert(change.clear_pixels && change.publish && !state.visible && !state.has_pixels);
  change = state.Update(false, {});
  assert(!change.clear_pixels && !change.publish);
  change = state.Update(true, {10,20,200,100});
  assert(change.clear_pixels && change.publish && !state.has_pixels);
  // A popup barrier must retain the final clean view even before popup pixels arrive.
  state.has_pixels = false;
  change = state.Update(false, {});
  assert(change.publish && !state.visible);
  std::cout << "GPU popup clipping and lifecycle tests passed\n";
}
