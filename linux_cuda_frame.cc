#include "linux_cuda_frame.h"
#include "gpu_frame_layout.h"
#include "gpu_popup_layout.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <vector>
#include <cstring>

namespace {
bool extension(const char* list, const char* name) {
  if (!list) return false;
  const size_t n = strlen(name);
  for (const char* p = list; (p = strstr(p, name)); p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
  return false;
}
struct CudaScope {
  bool active;
  explicit CudaScope(GstCudaContext* context) : active(gst_cuda_context_push(context)) {}
  ~CudaScope() { if (active) gst_cuda_context_pop(nullptr); }
};
bool cuda_ok(CUresult result, const char* operation, std::string& error) {
  if (result == CUDA_SUCCESS) return true;
  error = std::string(operation) + " failed: CUDA " + std::to_string(result);
  return false;
}
GLuint make_program(const char* fragment, std::string& error) {
  const char* vertex = "#version 330 core\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
  GLuint shaders[2] = {glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER)};
  const char* sources[2] = {vertex, fragment};
  GLuint program = glCreateProgram();
  bool compiled = true;
  for (int i = 0; i < 2; ++i) {
    glShaderSource(shaders[i], 1, &sources[i], nullptr); glCompileShader(shaders[i]);
    GLint ok = 0; glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
    compiled = compiled && ok; glAttachShader(program, shaders[i]);
  }
  glLinkProgram(program);
  for (GLuint shader : shaders) glDeleteShader(shader);
  GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!compiled || !linked) {
    glDeleteProgram(program); error = "GPU shader compilation failed"; return 0;
  }
  return program;
}
}

struct LinuxCudaFrame::Impl {
  GstCudaContext* cuda;
  GstBufferPool* pool = nullptr;
  GstCudaStream* stream = nullptr;
  GstVideoInfo info;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  GLuint texture = 0, framebuffer = 0, program = 0, vao = 0;
  CUgraphicsResource resource = nullptr;
  PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC image_storage = nullptr;
  bool modifiers = false;
  bool initialized = false;
  bool have_view = false;
  cef_gpu::PopupState popup;
  GLuint popup_texture = 0, popup_framebuffer = 0;
  GLuint composite_texture = 0, composite_framebuffer = 0, composite_program = 0;
  CUgraphicsResource composite_resource = nullptr;
  std::string setup_error;

  Impl(GstCudaContext* c, const GstVideoInfo& i, GstCaps* caps)
      : cuda(GST_CUDA_CONTEXT(gst_object_ref(c))), info(i) {
    stream = gst_cuda_stream_new(cuda);
    if (!stream) { setup_error = "cannot create owned CUDA copy stream"; return; }
    pool = gst_cuda_buffer_pool_new(cuda);
    GstStructure* config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, info.size, 0, 0);
    gst_buffer_pool_config_set_cuda_stream(config, stream);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    if (!gst_buffer_pool_set_config(pool, config) || !gst_buffer_pool_set_active(pool, TRUE))
      setup_error = "cannot configure CUDA output pool";
  }

  bool Init(std::string& error) {
    if (!setup_error.empty()) { error = setup_error; return false; }
    if (context != EGL_NO_CONTEXT) {
      if (!initialized) { error = "previous EGL initialization failed"; return false; }
      if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) return true;
      error = "cannot make EGL context current";
      return false;
    }
    auto query_devices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto device_attrib = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglGetProcAddress("eglQueryDeviceAttribEXT"));
    auto platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!query_devices || !device_attrib || !platform_display) {
      error = "EGL device selection extensions unavailable"; return false;
    }
    EGLDeviceEXT devices[32];
    EGLint count = 0;
    guint device_id = 0;
    g_object_get(cuda, "cuda-device-id", &device_id, nullptr);
    if (!query_devices(32, devices, &count)) { error = "cannot enumerate EGL devices"; return false; }
    for (int i = 0; i < count; ++i) {
      EGLAttrib id = -1;
      if (device_attrib(devices[i], EGL_CUDA_DEVICE_NV, &id) && id == static_cast<EGLAttrib>(device_id)) {
        display = platform_display(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr); break;
      }
    }
    EGLint major, minor;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
      error = "cannot initialize EGL on the negotiated CUDA device"; return false;
    }
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    modifiers = extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    if (!extension(extensions, "EGL_EXT_image_dma_buf_import") ||
        !extension(extensions, "EGL_KHR_surfaceless_context") || !eglBindAPI(EGL_OPENGL_API)) {
      error = "EGL DMA-BUF import or surfaceless OpenGL unavailable"; return false;
    }
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &configs) || configs == 0) {
      error = "cannot select EGL OpenGL config"; return false;
    }
    const EGLint ctx_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
      error = "cannot create OpenGL 3.3 EGL context"; return false;
    }
    image_storage = reinterpret_cast<PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC>(eglGetProcAddress("glEGLImageTargetTexStorageEXT"));
    if (!image_storage) { error = "GL_EXT_EGL_image_storage unavailable"; return false; }
    // Texture lookup resolves the DMA-BUF format. Store BGRA byte order for CUDA.
    const char* fs = "#version 330 core\nuniform sampler2D frame;uniform ivec2 origin;out vec4 color;void main(){color=texelFetch(frame,ivec2(gl_FragCoord.xy)+origin,0).bgra;}";
    program = make_program(fs, error);
    if (!program) return false;
    glGenVertexArrays(1, &vao);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, info.width, info.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) {
      error = "cannot allocate owned OpenGL staging frame"; return false;
    }
    CudaScope current(cuda);
    if (!current.active) { error = "cannot push CUDA context"; return false; }
    initialized = cuda_ok(CuGraphicsGLRegisterImage(&resource, texture, GL_TEXTURE_2D,
        CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "register staging GL texture", error);
    return initialized;
  }

  GstBuffer* Output(CUarray source, std::string& error) {
    if (!setup_error.empty()) { error = setup_error; return nullptr; }
    GstBuffer* output = nullptr;
    GstFlowReturn flow = gst_buffer_pool_acquire_buffer(pool, &output, nullptr);
    if (flow != GST_FLOW_OK) { error = "cannot acquire CUDA frame: " + std::string(gst_flow_get_name(flow)); return nullptr; }
    GstMemory* memory = gst_buffer_peek_memory(output, 0);
    GstMapInfo map = GST_MAP_INFO_INIT;
    if (!gst_is_cuda_memory(memory) || !gst_memory_map(memory, &map, GstMapFlags(GST_MAP_WRITE | GST_MAP_CUDA))) {
      error = "output is not writable CUDA memory"; gst_buffer_unref(output); return nullptr;
    }
    auto* cm = GST_CUDA_MEMORY_CAST(memory);
    CudaScope current(cuda);
    bool ok = current.active;
    if (!ok) error = "cannot push CUDA context";
    if (ok && source) {
      CUDA_MEMCPY2D copy = {};
      copy.srcMemoryType = CU_MEMORYTYPE_ARRAY; copy.srcArray = source;
      copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.dstDevice = reinterpret_cast<CUdeviceptr>(map.data) + cm->info.offset[0];
      copy.dstPitch = cm->info.stride[0]; copy.WidthInBytes = size_t(info.width) * 4; copy.Height = info.height;
      ok = cuda_ok(CuMemcpy2DAsync(&copy, gst_cuda_stream_get_handle(stream)), "copy staging array into owned CUDA buffer", error);
    } else if (ok) {
      ok = cuda_ok(CuMemsetD2D8Async(reinterpret_cast<CUdeviceptr>(map.data) + cm->info.offset[0],
          cm->info.stride[0], 0, size_t(info.width) * 4, info.height, gst_cuda_stream_get_handle(stream)), "clear CUDA frame", error);
    }
    // Complete the owned copy before the CEF callback releases its borrowed frame.
    if (ok) ok = cuda_ok(CuStreamSynchronize(gst_cuda_stream_get_handle(stream)), "complete CUDA frame", error);
    gst_memory_unmap(memory, &map);
    if (!ok) { gst_buffer_unref(output); return nullptr; }
    return output;
  }

  // Restore any caller context, including failed initialization.
  struct Release {
    Impl& p;
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    ~Release() {
      if (display != EGL_NO_DISPLAY) eglMakeCurrent(display, draw, read, context);
      else if (p.display != EGL_NO_DISPLAY) eglMakeCurrent(p.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
  };

  bool Surface(GLuint& target, GLuint& fbo, int width, int height, std::string& error) {
    glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) {
      error = "cannot allocate GPU popup surface"; return false;
    }
    return true;
  }

  void ClearPopup() {
    CudaScope current(cuda);
    if (composite_resource && current.active) CuGraphicsUnregisterResource(composite_resource);
    composite_resource = nullptr;
    if (popup_framebuffer) glDeleteFramebuffers(1, &popup_framebuffer);
    if (popup_texture) glDeleteTextures(1, &popup_texture);
    if (composite_framebuffer) glDeleteFramebuffers(1, &composite_framebuffer);
    if (composite_texture) glDeleteTextures(1, &composite_texture);
    if (composite_program) { glUseProgram(0); glDeleteProgram(composite_program); }
    popup_framebuffer = popup_texture = composite_framebuffer = composite_texture = composite_program = 0;
    popup.has_pixels = false;
  }

  GstBuffer* Emit(CUgraphicsResource source, std::string& error) {
    CudaScope current(cuda);
    if (!current.active) { error = "cannot push CUDA context"; return nullptr; }
    CUstream handle = gst_cuda_stream_get_handle(stream);
    if (!cuda_ok(CuGraphicsMapResources(1, &source, handle), "map staging texture", error)) return nullptr;
    CUarray array = nullptr;
    GstBuffer* output = nullptr;
    if (cuda_ok(CuGraphicsSubResourceGetMappedArray(&array, source, 0, 0), "get staging array", error))
      output = Output(array, error);
    if (!cuda_ok(CuGraphicsUnmapResources(1, &source, handle), "unmap staging texture", error)) {
      if (output) gst_buffer_unref(output);
      output = nullptr;
    }
    if (!cuda_ok(CuStreamSynchronize(handle), "complete staging unmap", error)) {
      if (output) gst_buffer_unref(output);
      output = nullptr;
    }
    return output;
  }

  GstBuffer* Compose(std::string& error) {
    if (!have_view) return nullptr;
    const auto clip = cef_gpu::clip_popup(info.width, info.height, popup.rect);
    if (!popup.visible || !popup.has_pixels || clip.width == 0) return Emit(resource, error);
    if (!composite_resource) {
      if (!Surface(composite_texture, composite_framebuffer, info.width, info.height, error)) return nullptr;
      // Both owned textures store BGRA components. Premultiplied source-over
      // keeps their byte order and avoids a straight-alpha conversion.
      const char* fs = "#version 330 core\nuniform sampler2D base_frame;uniform sampler2D popup_frame;uniform ivec4 bounds;uniform ivec2 popup_origin;out vec4 color;void main(){ivec2 p=ivec2(gl_FragCoord.xy);vec4 b=texelFetch(base_frame,p,0);color=b;if(p.x>=bounds.x&&p.y>=bounds.y&&p.x<bounds.x+bounds.z&&p.y<bounds.y+bounds.w){vec4 a=texelFetch(popup_frame,p-bounds.xy+popup_origin,0);color=a+b*(1.0-a.a);}}";
      composite_program = make_program(fs, error);
      if (!composite_program) return nullptr;
      CudaScope current(cuda);
      if (!current.active) { error = "cannot push CUDA context"; return nullptr; }
      if (!cuda_ok(CuGraphicsGLRegisterImage(&composite_resource, composite_texture, GL_TEXTURE_2D,
          CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "register popup composite texture", error)) return nullptr;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, composite_framebuffer);
    glViewport(0, 0, info.width, info.height);
    glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(composite_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(composite_program, "base_frame"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, popup_texture);
    glUniform1i(glGetUniformLocation(composite_program, "popup_frame"), 1);
    glUniform4i(glGetUniformLocation(composite_program, "bounds"), clip.x, clip.y, clip.width, clip.height);
    glUniform2i(glGetUniformLocation(composite_program, "popup_origin"), clip.source_x, clip.source_y);
    glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
    GLenum result = glGetError();
    if (result != GL_NO_ERROR) { error = "GPU popup composition failed: GL " + std::to_string(result); return nullptr; }
    return Emit(composite_resource, error);
  }

  ~Impl() {
    if (context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
      ClearPopup();
      CudaScope current(cuda);
      if (resource && current.active) CuGraphicsUnregisterResource(resource);
      if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
      if (texture) glDeleteTextures(1, &texture);
      if (program) glDeleteProgram(program);
      if (vao) glDeleteVertexArrays(1, &vao);
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    // EGL device displays can be shared across source instances. Do not terminate
    // the process-wide display while another consumer uses it.
    if (pool) { gst_buffer_pool_set_active(pool, FALSE); gst_object_unref(pool); }
    gst_clear_cuda_stream(&stream);
    gst_object_unref(cuda);
  }
};

LinuxCudaFrame::LinuxCudaFrame(GstCudaContext* c, const GstVideoInfo& info, GstCaps* caps)
    : impl_(new Impl(c, info, caps)) {}
LinuxCudaFrame::~LinuxCudaFrame() = default;
GstBuffer* LinuxCudaFrame::Blank(std::string& error) { return impl_->Output(nullptr, error); }

GstBuffer* LinuxCudaFrame::Copy(const CefAcceleratedPaintInfo& frame, std::string& error) {
  return CopyFrame(frame, false, error);
}
GstBuffer* LinuxCudaFrame::CopyPopup(const CefAcceleratedPaintInfo& frame, std::string& error) {
  return CopyFrame(frame, true, error);
}
GstBuffer* LinuxCudaFrame::UpdatePopup(bool visible, const CefRect& rect, std::string& error) {
  auto& p = *impl_;
  const auto change = p.popup.Update(visible, {rect.x, rect.y, rect.width, rect.height});
  if (!change.clear_pixels && !change.publish) return nullptr;
  if (!p.initialized) return nullptr;
  Impl::Release release{p};
  if (!p.Init(error)) return nullptr;
  if (change.clear_pixels) p.ClearPopup();
  // Hide and resize restore the clean base immediately. No view repaint is needed.
  return change.publish ? p.Compose(error) : nullptr;
}

GstBuffer* LinuxCudaFrame::CopyFrame(const CefAcceleratedPaintInfo& frame, bool popup, std::string& error) {
  auto& p = *impl_;
  if (popup && (!p.popup.visible || p.popup.rect.width <= 0 || p.popup.rect.height <= 0)) return nullptr;
  if (!CEF_MEMBER_EXISTS(&frame, extra) || !CEF_MEMBER_EXISTS(&frame.extra, visible_rect)) {
    error = "CEF frame metadata is missing"; return nullptr;
  }
  const auto& coded = frame.extra.coded_size;
  const auto& visible = frame.extra.visible_rect;
  cef_gpu::Frame f{coded.width, coded.height, visible.x, visible.y, visible.width, visible.height,
      frame.plane_count, frame.planes[0].fd, frame.planes[0].stride, frame.planes[0].offset,
      frame.planes[0].size, frame.modifier,
      frame.format == CEF_COLOR_TYPE_BGRA_8888 ? cef_gpu::Format::BGRA :
      frame.format == CEF_COLOR_TYPE_RGBA_8888 ? cef_gpu::Format::RGBA : cef_gpu::Format::Unsupported};
  const int width = popup ? p.popup.rect.width : p.info.width;
  const int height = popup ? p.popup.rect.height : p.info.height;
  // A popup from the preceding size can arrive after OnPopupSize or view resize.
  if (popup && (f.visible_width != width || f.visible_height != height)) return nullptr;
  cef_gpu::Layout layout{};
  if (const char* invalid = cef_gpu::validate(f, width, height, &layout)) {
    error = invalid; return nullptr;
  }
  Impl::Release release{p};
  if (!p.Init(error)) return nullptr;
  if (popup && !p.popup_texture && !p.Surface(p.popup_texture, p.popup_framebuffer, width, height, error)) return nullptr;
  std::vector<EGLint> attributes = {EGL_WIDTH, f.width, EGL_HEIGHT, f.height,
      EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(layout.drm_fourcc),
      EGL_DMA_BUF_PLANE0_FD_EXT, f.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(f.offset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(f.stride)};
  // DRM_FORMAT_MOD_INVALID requests implicit modifier selection, not a literal modifier.
  constexpr uint64_t invalid_modifier = (uint64_t(1) << 56) - 1;
  if (f.modifier != invalid_modifier) {
    if (!p.modifiers && f.modifier != 0) { error = "explicit DMA-BUF modifiers unsupported"; return nullptr; }
    if (p.modifiers) attributes.insert(attributes.end(), {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        static_cast<EGLint>(f.modifier & 0xffffffff), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        static_cast<EGLint>(f.modifier >> 32)});
  }
  attributes.push_back(EGL_NONE);
  auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  if (!create_image || !destroy_image) { error = "EGL image entry points unavailable"; return nullptr; }
  EGLImageKHR image = create_image(p.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
  if (image == EGL_NO_IMAGE_KHR) { error = "DMA-BUF EGL import failed: " + std::to_string(eglGetError()); return nullptr; }
  GLuint imported = 0; glGenTextures(1, &imported); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, imported);
  p.image_storage(GL_TEXTURE_2D, image, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, popup ? p.popup_framebuffer : p.framebuffer);
  glViewport(0, 0, width, height);
  glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_FRAMEBUFFER_SRGB);
  glUseProgram(p.program); glUniform1i(glGetUniformLocation(p.program, "frame"), 0);
  glUniform2i(glGetUniformLocation(p.program, "origin"), f.x, f.y); glBindVertexArray(p.vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  // This completes consumer GL work. Producer synchronization remains an external
  // implicit-sync contract and requires the separate hardware acceptance tests.
  glFinish();
  GLenum gl_error = glGetError();
  glDeleteTextures(1, &imported); destroy_image(p.display, image);
  if (gl_error != GL_NO_ERROR) { error = "GPU staging copy failed: GL " + std::to_string(gl_error); return nullptr; }
  if (popup) p.popup.has_pixels = true;
  else p.have_view = true;
  return p.Compose(error);
}
