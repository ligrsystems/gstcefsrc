#include <cstdio>
#include <glib.h>
#include <sstream>
#include <string>
#include <mutex>
#include <memory>

#ifdef __APPLE__
#include <memory>
#include <string>
#include <vector>
#include <mach-o/dyld.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <include/base/cef_bind.h>
#include <include/base/cef_callback_helpers.h>
#include <include/wrapper/cef_closure_task.h>
#include <include/wrapper/cef_message_router.h>

#include "gstcefsrc.h"
#include "gstcefaudiometa.h"
#ifdef GST_CEF_ENABLE_CUDA
#include "cef_cuda_startup.h"
#include "linux_cuda_frame.h"
#include "gpu_pair_queue.h"
#include "gpu_frame_queue.h"
#include "gpu_frame_clock.h"
#endif
#ifdef _WIN32
#include "d3d11_texture_reader.h"
#include <objbase.h>
#endif
#ifdef __APPLE__
#include "gstcefloader.h"
#include "gstcefnsapplication.h"
#endif

#define GST_ELEMENT_PROGRESS(el, type, code, text)      \
G_STMT_START {                                          \
  gchar *__txt = _gst_element_error_printf text;        \
  gst_element_post_message (GST_ELEMENT_CAST (el),      \
      gst_message_new_progress (GST_OBJECT_CAST (el),   \
          GST_PROGRESS_TYPE_ ##type, code, __txt));     \
  g_free (__txt);                                       \
} G_STMT_END


GST_DEBUG_CATEGORY_STATIC (cef_src_debug);
#define GST_CAT_DEFAULT cef_src_debug

GST_DEBUG_CATEGORY_STATIC (cef_console_debug);
#ifdef GST_CEF_ENABLE_CUDA
GST_DEBUG_CATEGORY_STATIC (cef_cadence_debug);
#endif


#ifdef GST_CEF_ENABLE_CUDA
// The caller holds the object lock and runs on the CEF UI thread.
static bool gst_cef_src_apply_popup_locked(GstCefSrc* src, std::string& error) {
  GstBuffer* output = nullptr;
  if (src->cuda_diagnostic_pairs && src->cuda_popup_visible) {
    error = "native popups are outside the paired full-frame diagnostic contract";
  } else if (src->cuda_frame) {
    CefRect bounds(src->cuda_popup_x, src->cuda_popup_y, src->cuda_popup_width, src->cuda_popup_height);
    output = src->cuda_frame->UpdatePopup(src->cuda_popup_visible, bounds, error);
  }
  if (output) {
    ++src->cuda_publish_sequence;
    src->cuda_frames->Reset(&src->current_buffer, &src->cuda_selected_sequence,
        output, src->cuda_publish_sequence);
    GST_CAT_LOG_OBJECT(cef_cadence_debug, src, "cadence publish seq=%" G_GUINT64_FORMAT " monotonic_us=%" G_GINT64_FORMAT " kind=popup-state",
        src->cuda_publish_sequence, g_get_monotonic_time());
    gst_buffer_unref(output);
  }
  return error.empty();
}
#endif

#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_FPS_N 30
#define DEFAULT_FPS_D 1
#define DEFAULT_URL "https://www.google.com"
#define DEFAULT_GPU FALSE
#define DEFAULT_CHROMIUM_DEBUG_PORT -1
#define DEFAULT_LOG_SEVERITY LOGSEVERITY_INFO
#if !defined(__APPLE__) && defined(GST_CEF_USE_SANDBOX)
#define DEFAULT_SANDBOX TRUE
#else
#define DEFAULT_SANDBOX FALSE
#endif
#define DEFAULT_LISTEN_FOR_JS_SIGNALS FALSE

using CefStatus = enum : guint8 {
  // CEF was either unloaded successfully or not yet loaded.
  CEF_STATUS_NOT_LOADED = 0U,
  // Blocks other elements from initializing CEF is it's already in progress.
  CEF_STATUS_INITIALIZING = 1U << 1U,
  // CEF's initialization process has completed successfully.
  CEF_STATUS_INITIALIZED = 1U << 2U,
  // No CEF elements will be allowed to complete initialization.
  CEF_STATUS_FAILURE = 1U << 3U,
};

static CefStatus cef_status = CEF_STATUS_NOT_LOADED;
static const guint8 CEF_STATUS_MASK_INITIALIZED = CEF_STATUS_FAILURE | CEF_STATUS_INITIALIZED;
static const guint8 CEF_STATUS_MASK_TRANSITIONING = CEF_STATUS_INITIALIZING;

static GMutex init_lock;
static GCond init_cond;
static gboolean context_initialized = FALSE;

#ifdef __APPLE__
// On every timeout, the CEF event handler will be run in the context
// of the main thread's Cocoa event loop.
static CFRunLoopTimerRef workTimer_ = nullptr;
#else
static GThread *thread = nullptr;
#endif

#define GST_TYPE_CEF_LOG_SEVERITY_MODE \
  (gst_cef_log_severity_mode_get_type ())


static const GEnumValue log_severity_values[] = {
  {LOGSEVERITY_DEBUG, "debug / verbose cef log severity", "debug"},
  {LOGSEVERITY_INFO, "info cef log severity", "info"},
  {LOGSEVERITY_WARNING, "warning cef log severity", "warning"},
  {LOGSEVERITY_ERROR, "error cef log severity", "error"},
  {LOGSEVERITY_FATAL, "fatal cef log severity", "fatal"},
  {LOGSEVERITY_DISABLE, "disable cef log severity", "disable"},
  {0, NULL, NULL},
};

static GType
gst_cef_log_severity_mode_get_type (void)
{
  static GType type = 0;
  if (!type) {
    type = g_enum_register_static ("GstCefLogSeverityMode", log_severity_values);
  }
  return type;
}

static gint gst_cef_log_severity_from_str (const gchar *str)
{
  for (guint i = 0; i < sizeof(log_severity_values) / sizeof(GEnumValue); i++) {
    const gchar *nick = log_severity_values[i].value_nick;
    if (!nick) break;
    if (g_str_equal(str, nick)) {
      return log_severity_values[i].value;
    }
  }

  return -1;
}

static gboolean
gst_cef_switch_allows_comma_value (const gchar *name)
{
  return g_strcmp0 (name, "enable-features") == 0 ||
      g_strcmp0 (name, "disable-features") == 0;
}

enum
{
  PROP_0,
  PROP_URL,
  PROP_GPU,
#ifdef GST_CEF_ENABLE_CUDA
  PROP_CUDA_MEMORY,
  PROP_CUDA_DIAGNOSTIC_PAIRS,
  PROP_CUDA_PAIR_DELAY_US,
#endif
  PROP_CHROMIUM_DEBUG_PORT,
  PROP_PAINT_RATE,
  PROP_CHROME_EXTRA_FLAGS,
  PROP_SANDBOX,
  PROP_LISTEN_FOR_JS_SIGNAL,
  PROP_JS_FLAGS,
  PROP_LOG_SEVERITY,
  PROP_CEF_CACHE_LOCATION,
};

#define gst_cef_src_parent_class parent_class
G_DEFINE_TYPE (GstCefSrc, gst_cef_src, GST_TYPE_PUSH_SRC);

#include "gstcef_video_caps.h"
#define CEF_AUDIO_CAPS "audio/x-raw, format=F32LE, rate=[1, 2147483647], channels=[1, 2147483647], layout=interleaved"

static GstStaticPadTemplate gst_cef_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CEF_VIDEO_CAPS)
    );

gchar* get_plugin_base_path () {
  GstPlugin *plugin = gst_registry_find_plugin(gst_registry_get(), "cef");
  gchar* base_path = g_path_get_dirname(gst_plugin_get_filename(plugin));
  gst_object_unref(plugin);
  return base_path;
}


/** Cef Client */

/** Handlers */

// see https://bitbucket.org/chromiumembedded/cef-project/src/master/examples/message_router
// and https://bitbucket.org/chromiumembedded/cef/src/master/include/wrapper/cef_message_router.h
// for details of the message passing infrastructure in CEF
// Handle messages in the browser process.
class MessageHandler : public CefMessageRouterBrowserSide::Handler {
 public:
  explicit MessageHandler(GstCefSrc* src)
      : src(src) {}

  // Called due to gstSendMsg execution in ready_test.html.
  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<Callback> callback) override
  {
    if (!src) return false;

    // TODO: do we want to make the incoming payload json??
    bool success = false;

    if (request == "ready") {
      g_mutex_lock (&src->state_lock);
      if (src->state == CEF_SRC_WAITING_FOR_READY) {
        src->state = CEF_SRC_READY;
        g_cond_broadcast (&src->state_cond);
        success = true;
      } else {
        std::ostringstream error_msg;
        error_msg << "error: (" << request << ") - " <<
          "js ready signal sent with invalid cef state: " << cef_status;
        GST_WARNING_OBJECT(src, "%s", error_msg.str().c_str());
        success = false;
      }
      g_mutex_unlock (&src->state_lock);
    } else if (request == "eos") {
      if (src) {
        gst_element_send_event(GST_ELEMENT(src), gst_event_new_eos());
        success = true;
      }
    }

    // send json response back to js
    std::ostringstream response;
    response <<
      "{ " <<
        "\"success\": \"" << (success ? "true" : "false") << "\", " <<
        "\"cmd\": \"" << request << "\"" <<
      " }";
    if (success) {
      GST_INFO_OBJECT(
        src, "js signal processed successfully: %s", request.ToString().c_str()
      );
      callback->Success(response.str());
    } else {
      GST_WARNING_OBJECT(
        src, "js signal processing error: %s", request.ToString().c_str()
      );
      callback->Failure(0, response.str());
    }

    return true;
  }

 private:
  GstCefSrc* src;

  DISALLOW_COPY_AND_ASSIGN(MessageHandler);
};

class RenderHandler : public CefRenderHandler
{
  public:

    RenderHandler(GstCefSrc *src) :
        src (src)
    {
    }

    ~RenderHandler()
    {
    }

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override
    {
	  GST_LOG_OBJECT(src, "getting view rect");
      GST_OBJECT_LOCK (src);
      rect = CefRect(0, 0, src->vinfo.width ? src->vinfo.width : DEFAULT_WIDTH, src->vinfo.height ? src->vinfo.height : DEFAULT_HEIGHT);
      GST_OBJECT_UNLOCK (src);
    }

    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList &dirtyRects, const void * buffer, int w, int h) override
    {
#ifdef GST_CEF_ENABLE_CUDA
      if (src->cuda_memory) {
        GST_OBJECT_LOCK(src);
        if (src->cuda_stopping) { GST_OBJECT_UNLOCK(src); return; }
        const bool first_error = !src->cuda_failed;
        src->cuda_failed = TRUE;
        g_cond_broadcast(&src->cuda_pair_cond);
        GST_OBJECT_UNLOCK(src);
        if (first_error) GST_ELEMENT_ERROR(src, RESOURCE, FAILED,
            ("CPU paint received while CUDA output was requested"), (nullptr));
        return;
      }
#endif
      GstBuffer *new_buffer;
      guint target_width, target_height;
      gsize target_size, source_size, copy_size;

      GST_LOG_OBJECT (src, "painting, width / height: %d %d", w, h);

      GST_OBJECT_LOCK (src);
      target_width = src->vinfo.width > 0 ? src->vinfo.width : (w > 0 ? (guint) w : (guint) DEFAULT_WIDTH);
      target_height = src->vinfo.height > 0 ? src->vinfo.height : (h > 0 ? (guint) h : (guint) DEFAULT_HEIGHT);
      GST_OBJECT_UNLOCK (src);

      target_size = (gsize) target_width * (gsize) target_height * 4;
      source_size = (w > 0 && h > 0) ? ((gsize) w * (gsize) h * 4) : 0;
      copy_size = MIN (target_size, source_size);

      new_buffer = gst_buffer_new_allocate (NULL, target_size, NULL);
      gst_buffer_fill (new_buffer, 0, buffer, copy_size);
      if (copy_size < target_size)
        gst_buffer_memset (new_buffer, copy_size, 0, target_size - copy_size);

      GST_OBJECT_LOCK (src);
      gst_buffer_replace (&(src->current_buffer), new_buffer);
      gst_buffer_unref (new_buffer);
      GST_OBJECT_UNLOCK (src);

      GST_LOG_OBJECT (src, "done painting");
    }

#ifdef GST_CEF_ENABLE_CUDA
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override {
      GST_DEBUG_OBJECT(src, "Native popup visibility: %d", show);
      UpdatePopupState(show, nullptr);
    }
    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override {
      GST_DEBUG_OBJECT(src, "Native popup bounds: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
      UpdatePopupState(false, &rect);
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList& dirtyRects, const CefAcceleratedPaintInfo& info) override
    {
      if (!src->cuda_memory) return;
      GST_CAT_LOG_OBJECT(cef_cadence_debug, src, "cadence callback monotonic_us=%" G_GINT64_FORMAT " kind=%s",
          g_get_monotonic_time(), type == PET_POPUP ? "popup" : "view");
      GST_OBJECT_LOCK(src);
      GST_CAT_LOG_OBJECT(cef_cadence_debug, src, "cadence locked monotonic_us=%" G_GINT64_FORMAT " kind=%s",
          g_get_monotonic_time(), type == PET_POPUP ? "popup" : "view");
      if (!src->cuda_frame || src->cuda_failed || src->cuda_pair_flushing || src->cuda_stopping ||
          src->cuda_frames->ResumePending()) { GST_OBJECT_UNLOCK(src); return; }
      if (src->cuda_diagnostic_pairs && !src->cuda_pairs->CanAcceptPair()) {
        GST_LOG_OBJECT(src, "Diagnostic queue full: skipping entire callback pair");
        GST_OBJECT_UNLOCK(src); return;
      }
      std::string error;
      GstBuffer* buffer = nullptr;
      if (type == PET_POPUP) {
        GST_LOG_OBJECT(src, "Accelerated native popup frame");
        if (src->cuda_diagnostic_pairs) error = "native popups are outside the paired full-frame diagnostic contract";
        else buffer = src->cuda_frame->CopyPopup(info, error);
        if (!buffer && error.empty()) { GST_OBJECT_UNLOCK(src); return; }
      } else if (CEF_MEMBER_EXISTS(&info, extra) && CEF_MEMBER_EXISTS(&info.extra, visible_rect) &&
                 (info.extra.visible_rect.width != src->vinfo.width || info.extra.visible_rect.height != src->vinfo.height)) {
        // An in-flight frame from the old size can arrive after renegotiation.
        GST_DEBUG_OBJECT(src, "Skipping old-size accelerated frame");
        GST_OBJECT_UNLOCK(src); return;
      } else {
        if (CEF_MEMBER_EXISTS(&info, extra) && CEF_MEMBER_EXISTS(&info.extra, visible_rect))
        GST_LOG_OBJECT(src, "Accelerated input: format=%d planes=%d coded=%dx%d visible=%d,%d %dx%d stride=%u offset=%" G_GUINT64_FORMAT " modifier=%" G_GUINT64_FORMAT,
            info.format, info.plane_count, info.extra.coded_size.width, info.extra.coded_size.height,
            info.extra.visible_rect.x, info.extra.visible_rect.y, info.extra.visible_rect.width,
            info.extra.visible_rect.height, info.planes[0].stride, info.planes[0].offset, info.modifier);
        buffer = src->cuda_frame->Copy(info, error);
        if (buffer && src->cuda_diagnostic_pairs) {
          // CEF still owns this callback. Re-import the same borrowed frame after
          // the delay; copying A's owned memory would hide producer readiness bugs.
          if (src->cuda_pair_delay_us) g_usleep(src->cuda_pair_delay_us);
          GstBuffer* second = src->cuda_frame->Copy(info, error);
          if (!second || !src->cuda_pairs->Push(buffer, second)) {
            if (second) error = "cannot enqueue a complete diagnostic pair";
            gst_buffer_unref(buffer);
            buffer = nullptr;
          }
          if (second) gst_buffer_unref(second);
          g_cond_signal(&src->cuda_pair_cond);
        }
      }
      if (buffer) {
        if (!src->cuda_diagnostic_pairs) {
          ++src->cuda_publish_sequence;
          const guint64 dropped = src->cuda_frames->Push(buffer, src->cuda_publish_sequence);
          if (dropped) GST_CAT_LOG_OBJECT(cef_cadence_debug, src, "cadence drop seq=%" G_GUINT64_FORMAT, dropped);
          GST_CAT_LOG_OBJECT(cef_cadence_debug, src, "cadence publish seq=%" G_GUINT64_FORMAT " monotonic_us=%" G_GINT64_FORMAT " kind=%s",
              src->cuda_publish_sequence, g_get_monotonic_time(), type == PET_POPUP ? "popup" : "view");
        }
        gst_buffer_unref(buffer);
        GST_LOG_OBJECT(src, "Owned CUDA frame: format=%d coded=%dx%d visible=%d,%d %dx%d stride=%u offset=%" G_GUINT64_FORMAT " modifier=%" G_GUINT64_FORMAT,
            info.format, info.extra.coded_size.width, info.extra.coded_size.height,
            info.extra.visible_rect.x, info.extra.visible_rect.y, info.extra.visible_rect.width,
            info.extra.visible_rect.height, info.planes[0].stride, info.planes[0].offset, info.modifier);
      } else {
        src->cuda_failed = TRUE;
        g_cond_broadcast(&src->cuda_pair_cond);
      }
      GST_OBJECT_UNLOCK(src);
      if (!buffer) GST_ELEMENT_ERROR(src, RESOURCE, FAILED,
          ("CUDA browser frame transfer failed"), ("%s", error.c_str()));
    }
#endif

#ifdef _WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                            PaintElementType type,
                            const RectList& dirtyRects,
                            const CefAcceleratedPaintInfo& info) override
    {
      if (type != PET_VIEW) return;

      if (!src->texture_reader) {
        src->texture_reader = new D3D11TextureReader();
      }

      int width = GST_VIDEO_INFO_WIDTH(&src->vinfo);
      int height = GST_VIDEO_INFO_HEIGHT(&src->vinfo);

      if (width <= 0 || height <= 0) return;

      const void* pixels = nullptr;
      int stride = 0;

      if (!src->texture_reader->ReadTexture(
              info.shared_texture_handle, width, height, &pixels, &stride)) {
        GST_ERROR_OBJECT(src, "D3D11 shared texture readback failed");
        return;
      }

      // Allocate GstBuffer and copy pixels row-by-row (GPU staging textures
      // may have row padding, so stride can exceed width*4).
      gsize row_bytes = (gsize)width * 4;
      gsize packed_size = row_bytes * height;
      GstBuffer* new_buffer = gst_buffer_new_allocate(nullptr, packed_size, nullptr);
      {
          GstMapInfo map;
          if (!new_buffer || !gst_buffer_map(new_buffer, &map, GST_MAP_WRITE)) {
              GST_ERROR_OBJECT(src, "Failed to map GstBuffer for GPU frame");
              if (new_buffer) gst_buffer_unref(new_buffer);
              src->texture_reader->Release();
              return;
          }
          const uint8_t* src_row = static_cast<const uint8_t*>(pixels);
          uint8_t* dst_row = map.data;
          for (int y = 0; y < height; y++, src_row += stride, dst_row += row_bytes)
              memcpy(dst_row, src_row, row_bytes);
          gst_buffer_unmap(new_buffer, &map);
      }

      src->texture_reader->Release();

      GST_OBJECT_LOCK (src);
      gst_buffer_replace (&(src->current_buffer), new_buffer);
      gst_buffer_unref (new_buffer);
      GST_OBJECT_UNLOCK (src);

      GST_LOG_OBJECT(src, "OnAcceleratedPaint frame: %dx%d stride=%d", width, height, stride);
    }
#endif

  private:

#ifdef GST_CEF_ENABLE_CUDA
    void UpdatePopupState(bool show, const CefRect* rect) {
      if (!src->cuda_memory) return;
      GST_OBJECT_LOCK(src);
      if (src->cuda_failed || src->cuda_stopping) {
        GST_OBJECT_UNLOCK(src); return;
      }
      const bool popup_changed = rect
          ? (rect->x != src->cuda_popup_x || rect->y != src->cuda_popup_y ||
             rect->width != src->cuda_popup_width || rect->height != src->cuda_popup_height)
          : (show != bool(src->cuda_popup_visible));
      if (popup_changed) src->cuda_frames->Clear();
      if (rect) {
        src->cuda_popup_x = rect->x; src->cuda_popup_y = rect->y;
        src->cuda_popup_width = rect->width; src->cuda_popup_height = rect->height;
      } else {
        src->cuda_popup_visible = show;
        src->cuda_popup_x = src->cuda_popup_y = src->cuda_popup_width = src->cuda_popup_height = 0;
      }
      // Retain desired state during flushing. The guarded UI resume task applies it.
      if (src->cuda_pair_flushing || src->cuda_frames->ResumePending()) {
        GST_OBJECT_UNLOCK(src); return;
      }
      std::string error;
      gst_cef_src_apply_popup_locked(src, error);
      if (!error.empty()) {
        src->cuda_failed = TRUE;
        g_cond_broadcast(&src->cuda_pair_cond);
      }
      GST_LOG_OBJECT(src, "Native popup: visible=%d rect=%d,%d %dx%d", src->cuda_popup_visible,
          src->cuda_popup_x, src->cuda_popup_y, src->cuda_popup_width, src->cuda_popup_height);
      GST_OBJECT_UNLOCK(src);
      if (!error.empty()) GST_ELEMENT_ERROR(src, RESOURCE, FAILED,
          ("CUDA popup update failed"), ("%s", error.c_str()));
    }
#endif

    GstCefSrc *src;

    IMPLEMENT_REFCOUNTING(RenderHandler);
};

class AudioHandler : public CefAudioHandler
{
  public:

    AudioHandler(GstCefSrc *src) :
        src (src)
    {
    }

    ~AudioHandler()
    {
    }

    /* Called from BrowserClient::OnBeforeClose (CEF UI thread) before the
     * GstCefSrc element is torn down. Takes mLock so it waits for any in-flight
     * audio-thread callback to finish, then drops the element pointer so later
     * callbacks (CEF's audio thread can fire them after the browser closes)
     * become no-ops instead of dereferencing freed memory. */
    void Invalidate()
    {
      std::lock_guard<std::mutex> lock(mLock);
      src = nullptr;
    }

  void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                            const CefAudioParameters& params,
                            int channels) override
  {
    std::lock_guard<std::mutex> lock(mLock);
    if (!src)
      return;

    GstStructure *s = gst_structure_new ("cef-audio-stream-start",
        "channels", G_TYPE_INT, channels,
        "rate", G_TYPE_INT, params.sample_rate,
        nullptr);
    GstEvent *event = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, s);

    mRate = params.sample_rate;
    mChannels = channels;

    GST_OBJECT_LOCK (src);
    src->audio_events = g_list_append (src->audio_events, event);
    GST_OBJECT_UNLOCK (src);
  }

  void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser,
                           const float** data,
                           int frames,
                           int64_t pts) override
  {
    GstBuffer *buf;
    GstMapInfo info;
    gint i, j;

    std::lock_guard<std::mutex> lock(mLock);
    if (!src)
      return;

    GST_LOG_OBJECT (src, "Handling audio stream packet with %d frames", frames);

    buf = gst_buffer_new_allocate (NULL, mChannels * frames * 4, NULL);

    gst_buffer_map (buf, &info, GST_MAP_WRITE);
    for (i = 0; i < mChannels; i++) {
      gfloat *cdata = (gfloat *) data[i];

      for (j = 0; j < frames; j++) {
        memcpy (info.data + j * 4 * mChannels + i * 4, &cdata[j], 4);
      }
    }
    gst_buffer_unmap (buf, &info);

    GST_OBJECT_LOCK (src);

    GST_BUFFER_DURATION (buf) = gst_util_uint64_scale (frames, GST_SECOND, mRate);

    if (!src->audio_buffers) {
      src->audio_buffers = gst_buffer_list_new();
    }

    gst_buffer_list_add (src->audio_buffers, buf);
    GST_OBJECT_UNLOCK (src);

    GST_LOG_OBJECT (src, "Handled audio stream packet");
  }

  void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override
  {
  }

  void OnAudioStreamError(CefRefPtr<CefBrowser> browser,
                          const CefString& message) override {
    std::lock_guard<std::mutex> lock(mLock);
    if (!src)
      return;
    GST_WARNING_OBJECT (src, "Audio stream error: %s", message.ToString().c_str());
  }

  private:

    GstCefSrc *src;
    gint mRate;
    gint mChannels;
    /* Guards `src` against the teardown race: audio callbacks run on CEF's
     * audio thread and can fire while/after the element is being finalized. */
    std::mutex mLock;
    IMPLEMENT_REFCOUNTING(AudioHandler);
};

class DisplayHandler : public CefDisplayHandler {
public:
  DisplayHandler(GstCefSrc *src) : src(src) {}

  ~DisplayHandler() = default;

  virtual bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString &message, const CefString &source, int line) override {
    GstDebugLevel gst_level = GST_LEVEL_NONE;
    switch (level) {
    case LOGSEVERITY_DEFAULT:
    case LOGSEVERITY_INFO:
      gst_level = GST_LEVEL_INFO;
      break;
    case LOGSEVERITY_DEBUG:
      gst_level = GST_LEVEL_DEBUG;
      break;
    case LOGSEVERITY_WARNING:
      gst_level = GST_LEVEL_WARNING;
      break;
    case LOGSEVERITY_ERROR:
    case LOGSEVERITY_FATAL:
      gst_level = GST_LEVEL_ERROR;
      break;
    case LOGSEVERITY_DISABLE:
      gst_level = GST_LEVEL_NONE;
      break;
    };
    GST_CAT_LEVEL_LOG (cef_console_debug, gst_level, src, "%s:%d %s", source.ToString().c_str(), line,
      message.ToString().c_str());
    return false;
  }

private:
  GstCefSrc *src;
  IMPLEMENT_REFCOUNTING(DisplayHandler);
};

static void
gst_cef_load_url_on_ui_thread (CefRefPtr<CefBrowser> browser, std::string url)
{
  CEF_REQUIRE_UI_THREAD();
  if (!browser) return;
  CefRefPtr<CefFrame> frame = browser->GetMainFrame();
  if (frame) frame->LoadURL(url);
}

static void
gst_cef_close_browser_on_ui_thread (CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();
  if (!browser) return;
  browser->GetHost()->CloseBrowser(true);
}

class BrowserClient :
  public CefClient,
  public CefLifeSpanHandler,
  public CefRequestHandler
{
  public:

    BrowserClient(GstCefSrc *src) : src(src)
    {

      this->render_handler = new RenderHandler(src);
      this->audio_handler = new AudioHandler(src);
      this->display_handler = new DisplayHandler(src);
    }

    // CefClient Methods:
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override
    {
      return this;
    }

    virtual CefRefPtr<CefRenderHandler> GetRenderHandler() override
    {
      return render_handler;
    }

    virtual CefRefPtr<CefAudioHandler> GetAudioHandler() override
    {
      return audio_handler;
    }

    virtual CefRefPtr<CefRequestHandler> GetRequestHandler() override
    {
      return this;
    }

    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override
    {
      return display_handler;
    }

    bool OnProcessMessageReceived(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefProcessId source_process,
      CefRefPtr<CefProcessMessage> message
    ) override
    {
      CEF_REQUIRE_UI_THREAD();

      return browser_msg_router_
        ? browser_msg_router_->OnProcessMessageReceived(
          browser,
          frame,
          source_process,
          message
        )
        : false;
    }

    // CefLifeSpanHandler Methods:
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
      CEF_REQUIRE_UI_THREAD();

      if (src->listen_for_js_signals && !browser_msg_router_) {
        // Create the browser-side router for query handling.
        CefMessageRouterConfig config;
        config.js_query_function = "gstSendMsg";
        config.js_cancel_function = "gstCancelMsg";
        browser_msg_router_ = CefMessageRouterBrowserSide::Create(config);

        // Register handlers with the router.
        browser_msg_handler_.reset(new MessageHandler(src));
        browser_msg_router_->AddHandler(browser_msg_handler_.get(), false);
      }

      browser->GetHost()->SetAudioMuted(true);

      g_mutex_lock (&src->state_lock);
      src->browser = browser;
      src->state = src->listen_for_js_signals ? CEF_SRC_WAITING_FOR_READY : CEF_SRC_OPEN;
      g_cond_signal (&src->state_cond);
      g_mutex_unlock(&src->state_lock);
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
    {
      /* Drop the audio handler's element pointer before marking the browser
       * closed: gst_cef_src_stop() waits on CEF_SRC_CLOSED before the element is
       * freed, so invalidating here guarantees no late audio-thread callback
       * dereferences freed memory (see AudioHandler::Invalidate). */
      if (audio_handler)
        audio_handler->Invalidate();

      g_mutex_lock (&src->state_lock);
      src->browser = nullptr;
      src->state = CEF_SRC_CLOSED;
      g_cond_signal (&src->state_cond);
      g_mutex_unlock(&src->state_lock);
    }

    // CefRequestHandler methods:
    bool OnBeforeBrowse(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool user_gesture,
      bool is_redirect
    ) override
    {
      CEF_REQUIRE_UI_THREAD();

      if (browser_msg_router_) browser_msg_router_->OnBeforeBrowse(browser, frame);
      return false;
    }

  virtual void OnRenderProcessTerminated(
    CefRefPtr<CefBrowser> browser,
    TerminationStatus status,
    int error_code,
    const CefString& error_string) override
    {
      CEF_REQUIRE_UI_THREAD();
      GST_WARNING_OBJECT (src, "Render subprocess terminated, reloading URL!");
      if (browser_msg_router_) browser_msg_router_->OnRenderProcessTerminated(browser);
      browser->Reload();
    }

    // Custom methods:
    void MakeBrowser(int)
    {
      CefWindowInfo window_info;
      CefBrowserSettings browser_settings;

      // CefStructBase::init() sets window_info.size = sizeof(cef_window_info_t)
      // Do NOT override with sizeof(CefWindowInfo) -- the C++ wrapper is larger
      // due to CefStructBase overhead, and CEF rejects mismatched sizes.
      window_info.SetAsWindowless(0);
#ifdef GST_CEF_ENABLE_CUDA
      window_info.shared_texture_enabled = src->cuda_memory;
#endif

#ifdef _WIN32
      if (src->gpu) {
        window_info.shared_texture_enabled = true;
        GST_INFO_OBJECT(src, "Hardware acceleration enabled: shared_texture_enabled=true");
      }
#endif

      CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info,
        this,
        std::string(src->url),
        browser_settings,
        nullptr,
        nullptr
      );

      if (!browser) {
        GST_ERROR_OBJECT (src, "Failed to create CEF browser (CreateBrowserSync returned null)");
        g_mutex_lock (&src->state_lock);
        src->state = CEF_SRC_OPEN;
        g_cond_signal (&src->state_cond);
        g_mutex_unlock(&src->state_lock);
      }
      // On success, OnAfterCreated will fire and set src->browser
    }

  private:
    // Handles the browser side of query routing.
    CefRefPtr<CefMessageRouterBrowserSide> browser_msg_router_;
    std::unique_ptr<CefMessageRouterBrowserSide::Handler> browser_msg_handler_;

    CefRefPtr<CefRenderHandler> render_handler;
    /* Concrete type (not CefAudioHandler) so OnBeforeClose can call Invalidate() */
    CefRefPtr<AudioHandler> audio_handler;
    CefRefPtr<CefDisplayHandler> display_handler;

  public:
    GstCefSrc *src;

    IMPLEMENT_REFCOUNTING(BrowserClient);
};


/** Browser App methods */

BrowserApp::BrowserApp(GstCefSrc *src) : src(src)
{
}

void BrowserApp::OnContextInitialized()
{
  CEF_REQUIRE_UI_THREAD();
  GST_INFO_OBJECT(src, "CEF context initialized - message loop is running");

  g_mutex_lock (&init_lock);
  context_initialized = TRUE;
  g_cond_broadcast (&init_cond);
  g_mutex_unlock (&init_lock);
}

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler()
{
  return this;
}

#ifdef __APPLE__
void BrowserApp::OnScheduleMessagePumpWork(int64_t delay_ms)
{
  static const int64_t kMaxTimerDelay = 1000.0 / 60.0;

  if (workTimer_ != nullptr) {
    CFRunLoopTimerInvalidate(workTimer_);
    workTimer_ = nullptr;
  }

  if (delay_ms <= 0) {
    // Execute the work immediately.
    gst_cef_loop();

    // Schedule more work later.
    OnScheduleMessagePumpWork(kMaxTimerDelay);
  } else {
      int64_t timer_delay_ms = delay_ms;
      // Never wait longer than the maximum allowed time.
      if (timer_delay_ms > kMaxTimerDelay) timer_delay_ms = kMaxTimerDelay;

      workTimer_ = gst_cef_domessagework((double)timer_delay_ms * (1.0 / 1000.0));

      CFRunLoopAddTimer(CFRunLoopGetMain(), workTimer_, kCFRunLoopDefaultMode);
  }
}
#endif

void BrowserApp::OnBeforeCommandLineProcessing(const CefString &process_type,
                                               CefRefPtr<CefCommandLine> command_line)
{
    command_line->AppendSwitch("no-first-run");
    command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
    command_line->AppendSwitch("enable-media-stream");
    command_line->AppendSwitch("disable-dev-shm-usage"); /* https://github.com/GoogleChrome/puppeteer/issues/1834 */
    command_line->AppendSwitch("enable-begin-frame-scheduling"); /* https://bitbucket.org/chromiumembedded/cef/issues/1368 */

    bool gpu = src->gpu || (!!g_getenv ("GST_CEF_GPU_ENABLED"));
#ifdef GST_CEF_ENABLE_CUDA
    gpu = gpu || src->cuda_memory;
#endif

#ifdef __APPLE__
    command_line->AppendSwitch("off-screen-rendering-enabled");
    if (gpu) {
      GST_WARNING_OBJECT(src, "GPU rendering is known not to work on macOS. Disabling it now. See https://github.com/chromiumembedded/cef/issues/3322 and https://magpcss.org/ceforum/viewtopic.php?f=6&t=19397");
      gpu = FALSE;
    }
#endif

    if (!gpu) {
      // Optimize for no gpu usage
      command_line->AppendSwitch("disable-gpu");
      command_line->AppendSwitch("disable-gpu-compositing");
    }

    if (src->chromium_debug_port >= 0) {
      command_line->AppendSwitchWithValue("remote-debugging-port", g_strdup_printf ("%i", src->chromium_debug_port));
    }

    const gchar *extra_flags = src->chrome_extra_flags;
    if (!extra_flags) {
      extra_flags = g_getenv ("GST_CEF_CHROME_EXTRA_FLAGS");
    }

    if (extra_flags) {
      gchar **flags_list = g_strsplit (extra_flags, ",", -1);
      gchar *pending_switch = NULL;
      gchar *pending_value = NULL;
      guint i;

      for (i = 0; i < g_strv_length (flags_list); i++) {
        gchar *token = g_strstrip (flags_list[i]);
        if (!token || !*token) continue;

        gchar *equals = strchr (token, '=');
        if (equals != NULL) {
          *equals = '\0';
          const gchar *switch_name = token;
          const gchar *switch_value = equals + 1;

          if (pending_switch) {
            GST_INFO_OBJECT (src, "Adding switch with value %s=%s", pending_switch, pending_value);
            command_line->AppendSwitchWithValue (pending_switch, pending_value);
            g_free (pending_switch);
            g_free (pending_value);
            pending_switch = NULL;
            pending_value = NULL;
          }

          if (gst_cef_switch_allows_comma_value (switch_name)) {
            pending_switch = g_strdup (switch_name);
            pending_value = g_strdup (switch_value);
          } else {
            GST_INFO_OBJECT (src, "Adding switch with value %s=%s", switch_name, switch_value);
            command_line->AppendSwitchWithValue (switch_name, switch_value);
          }
        } else {
          if (pending_switch) {
            gchar *combined = g_strdup_printf ("%s,%s", pending_value, token);
            g_free (pending_value);
            pending_value = combined;
          } else {
            GST_INFO_OBJECT (src, "Adding flag %s", token);
            command_line->AppendSwitch (token);
          }
        }
      }

      if (pending_switch) {
        GST_INFO_OBJECT (src, "Adding switch with value %s=%s", pending_switch, pending_value);
        command_line->AppendSwitchWithValue (pending_switch, pending_value);
        g_free (pending_switch);
        g_free (pending_value);
      }

      g_strfreev (flags_list);
    }

#ifdef GST_CEF_ENABLE_CUDA
    // Apply after user flags, before the process-wide CEF initialization.
    gst_cef_apply_cuda_startup_defaults(src->cuda_memory, *command_line);
#endif
}


/** cefsrc (Gstreamer) methods */

#ifdef GST_CEF_ENABLE_CUDA
// Wait before borrowing the next completed publication. GstBaseSrc otherwise
// selects in create(), then waits with that older frame already in flight.
static GstFlowReturn gst_cef_src_wait_cuda_frame(GstCefSrc* src, GstClockTime* pts, bool* discont) {
  for (;;) {
    GST_OBJECT_LOCK(src);
    if (src->cuda_pair_flushing || src->cuda_stopping) { GST_OBJECT_UNLOCK(src); return GST_FLOW_FLUSHING; }
    if (src->cuda_failed) { GST_OBJECT_UNLOCK(src); return GST_FLOW_ERROR; }
    const int fps_n = src->vinfo.fps_n, fps_d = src->vinfo.fps_d;
    GST_OBJECT_UNLOCK(src);
    GstClock* clock = gst_element_get_clock(GST_ELEMENT(src));
    const GstClockTime base = gst_element_get_base_time(GST_ELEMENT(src));
    const auto result = src->cuda_clock->Wait(clock, base, fps_n, fps_d, pts, discont);
    GstClock* current_clock = gst_element_get_clock(GST_ELEMENT(src));
    const bool changed = current_clock != clock || base != gst_element_get_base_time(GST_ELEMENT(src));
    gst_clear_object(&current_clock);
    gst_clear_object(&clock);
    if (result == GpuFrameClock::Result::Cancelled) return GST_FLOW_FLUSHING;
    if (result == GpuFrameClock::Result::Retry || changed) {
      src->cuda_clock->Invalidate();
      continue;
    }
    if (result == GpuFrameClock::Result::Ready) return GST_FLOW_OK;
    GST_ELEMENT_ERROR(src, CORE, CLOCK, ("Cannot schedule a CUDA frame on the pipeline clock"), (nullptr));
    return GST_FLOW_ERROR;
  }
}
#endif

static GstFlowReturn gst_cef_src_create(GstPushSrc *push_src, GstBuffer **buf)
{
  GstCefSrc *src = GST_CEF_SRC (push_src);
  GList *tmp;
#ifdef GST_CEF_ENABLE_CUDA
  GstClockTime cuda_pts = GST_CLOCK_TIME_NONE;
  bool cuda_discont = false;
  if (src->cuda_memory && !src->cuda_diagnostic_pairs) {
    const GstFlowReturn result = gst_cef_src_wait_cuda_frame(src, &cuda_pts, &cuda_discont);
    if (result != GST_FLOW_OK) return result;
  }
#endif

  GST_OBJECT_LOCK (src);

#ifdef GST_CEF_ENABLE_CUDA
  if (src->cuda_memory && src->cuda_pair_flushing) { GST_OBJECT_UNLOCK(src); return GST_FLOW_FLUSHING; }
  if (src->cuda_diagnostic_pairs) {
    *buf = nullptr;
    const gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (!src->cuda_failed && !src->cuda_pair_flushing && !(*buf = src->cuda_pairs->Pop())) {
      if (!g_cond_wait_until(&src->cuda_pair_cond, GST_OBJECT_GET_LOCK(src), deadline)) {
        src->cuda_failed = TRUE;
        GST_OBJECT_UNLOCK(src);
        GST_ELEMENT_ERROR(src, RESOURCE, FAILED, ("Timed out waiting for a complete CUDA diagnostic pair"), (nullptr));
        return GST_FLOW_ERROR;
      }
    }
    if (src->cuda_pair_flushing) { GST_OBJECT_UNLOCK(src); return GST_FLOW_FLUSHING; }
    if (src->cuda_failed) { GST_OBJECT_UNLOCK(src); return GST_FLOW_ERROR; }
  }
#endif

  if (src->audio_events) {
    for (tmp = src->audio_events; tmp; tmp = tmp->next) {
      gst_pad_push_event (GST_BASE_SRC_PAD (src), (GstEvent *) tmp->data);
    }

    g_list_free (src->audio_events);
    src->audio_events = NULL;
  }

#ifdef GST_CEF_ENABLE_CUDA
  if (src->cuda_failed) { GST_OBJECT_UNLOCK(src); return GST_FLOW_ERROR; }
#endif
#ifdef GST_CEF_ENABLE_CUDA
  if (!src->cuda_diagnostic_pairs)
#endif
  {
#ifdef GST_CEF_ENABLE_CUDA
    if (src->cuda_memory) src->cuda_frames->Select(&src->current_buffer, &src->cuda_selected_sequence);
#endif
    g_assert (src->current_buffer);
    *buf = gst_buffer_copy (src->current_buffer);
  }

  if (src->audio_buffers) {
    gst_buffer_add_cef_audio_meta (*buf, src->audio_buffers);
    src->audio_buffers = NULL;
  }

#ifdef GST_CEF_ENABLE_CUDA
  if (GST_CLOCK_TIME_IS_VALID(cuda_pts)) {
    GST_BUFFER_PTS(*buf) = GST_BUFFER_DTS(*buf) = cuda_pts;
    if (cuda_discont) GST_BUFFER_FLAG_SET(*buf, GST_BUFFER_FLAG_DISCONT);
  } else
#endif
  GST_BUFFER_PTS (*buf) = gst_util_uint64_scale (src->n_frames, src->vinfo.fps_d * GST_SECOND, src->vinfo.fps_n);
  GST_BUFFER_DURATION (*buf) = gst_util_uint64_scale (GST_SECOND, src->vinfo.fps_d, src->vinfo.fps_n);
#ifdef GST_CEF_ENABLE_CUDA
  if (src->cuda_memory && !src->cuda_diagnostic_pairs)
    GST_CAT_LOG_OBJECT(cef_cadence_debug, src, "cadence select seq=%" G_GUINT64_FORMAT " frame=%" G_GUINT64_FORMAT " pts_ns=%" G_GUINT64_FORMAT " monotonic_us=%" G_GINT64_FORMAT,
        src->cuda_selected_sequence, src->n_frames, GST_BUFFER_PTS(*buf), g_get_monotonic_time());
#endif
  src->n_frames++;
  GST_OBJECT_UNLOCK (src);

  return GST_FLOW_OK;
}

/* Once we have started a first cefsrc for this process, we start
 * a UI thread and never shut it down. We could probably refine this
 * to stop and restart the thread as needed, but this updated approach
 * now no longer requires a main loop to be running, doesn't crash
 * when one is running either with CEF 86+, and allows for multiple
 * concurrent cefsrc instances.
 */
static gpointer
init_cef (GstCefSrc *src)
{
  /* Reset context_initialized for this CEF lifecycle */
  g_mutex_lock (&init_lock);
  context_initialized = FALSE;
  g_mutex_unlock (&init_lock);
#ifdef G_OS_WIN32
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HINSTANCE hInstance = GetModuleHandle(NULL);
  CefMainArgs args(hInstance);
#else
  CefMainArgs args(0, NULL);
#endif

  CefSettings settings;
  CefRefPtr<BrowserApp> app;
  CefWindowInfo window_info;
  CefBrowserSettings browserSettings;

  // pull in parameters from gst properties (deprecated) or environment
  cef_log_severity_t log_severity = src->log_severity;
  const gchar* log_severity_env = g_getenv ("GST_CEF_LOG_SEVERITY");
  if (log_severity_env) {
    gint severity = gst_cef_log_severity_from_str(log_severity_env);
    if (severity >= 0) {
      log_severity = (cef_log_severity_t) severity;
    }
  }

  const gchar *js_flags = src->js_flags;
  if (!js_flags) {
    js_flags = g_getenv ("GST_CEF_JS_FLAGS");
  }

  const gchar *cef_cache_location = src->cef_cache_location;
  if (!cef_cache_location) {
    cef_cache_location = g_getenv ("GST_CEF_CACHE_LOCATION");
  }

  bool sandbox = src->sandbox || (!!g_getenv ("GST_CEF_SANDBOX"));

  settings.no_sandbox = !sandbox;
  settings.windowless_rendering_enabled = true;
  settings.log_severity = log_severity;
  settings.multi_threaded_message_loop = false;
#ifdef __APPLE__
  settings.external_message_pump = true;
#endif

  GST_INFO_OBJECT(src, "Initializing CEF");

  gchar* base_path = get_plugin_base_path();

  // If not absolute path append to current_dir
  if (!g_path_is_absolute(base_path)) {
    gchar* current_dir = g_get_current_dir();

    gchar* old_base_path = base_path;
    base_path = g_build_filename(current_dir, base_path, nullptr);

    g_free(current_dir);
    g_free(old_base_path);
  }

#ifdef __APPLE__
  gchar* browser_subprocess_path = g_build_filename(base_path, "gstcefsubprocess.app/Contents/MacOS/gstcefsubprocess", nullptr);
#elif defined(_WIN32)
  gchar* browser_subprocess_path = g_build_filename(base_path, "gstcefsubprocess.exe", nullptr);
#else
  gchar* browser_subprocess_path = g_build_filename(base_path, "gstcefsubprocess", nullptr);
#endif
  if (const gchar *custom_subprocess_path = g_getenv ("GST_CEF_SUBPROCESS_PATH")) {
    g_setenv ("CEF_SUBPROCESS_PATH", browser_subprocess_path, TRUE);
    g_free (browser_subprocess_path);
    browser_subprocess_path = g_strdup (custom_subprocess_path);
  }

  GST_DEBUG_OBJECT(src, "CEF subprocess: %s", browser_subprocess_path);
  CefString(&settings.browser_subprocess_path).FromASCII(browser_subprocess_path);
  g_free(browser_subprocess_path);

#ifdef __APPLE__
  const std::string framework_folder = []() {
    std::string framework = gst_cef_get_framework_path(false);
    const auto split = framework.find_last_of('/');
    return framework.substr(0, split);
  }();
  GST_DEBUG_OBJECT(src, "CEF framework_dir_path: %s", framework_folder.c_str());
  CefString(&settings.framework_dir_path).FromString(framework_folder);
  const std::string main_bundle_folder = [&](){
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> host(size);
    _NSGetExecutablePath(host.data(), &size);
    auto host2 = std::unique_ptr<char>(realpath(host.data(), nullptr));
    GST_DEBUG_OBJECT(src, "Main executable path: %s", host2.get());
    assert(size != 0);
    std::string host3(host2.get());
    const auto split = host3.find("Contents/MacOS");
    assert(split != std::string::npos);
    return host3.substr(0, split);
  }();
  CefString(&settings.main_bundle_path).FromString(main_bundle_folder);
#endif

  gchar *locales_dir_path = g_build_filename(base_path, "locales", nullptr);
  CefString(&settings.locales_dir_path).FromASCII(locales_dir_path);

  // CEF 139 requires resources_dir_path to find .pak files
  CefString(&settings.resources_dir_path).FromASCII(base_path);

  if (js_flags != NULL) {
    CefString(&settings.javascript_flags).FromASCII(js_flags);
  }

  if (cef_cache_location != NULL) {
    CefString(&settings.cache_path).FromASCII(cef_cache_location);
    CefString(&settings.root_cache_path).FromASCII(cef_cache_location);
  }

  g_free(base_path);
  g_free(locales_dir_path);

  // Set root_cache_path (required by CEF 139)
  const gchar *root_cache_path = g_getenv("GST_CEF_ROOT_CACHE_PATH");
  if (root_cache_path) {
    CefString(&settings.root_cache_path).FromASCII(root_cache_path);
    GST_INFO_OBJECT(src, "Setting root_cache_path=%s", root_cache_path);
    if (!cef_cache_location) {
      // If cache_path not set, use root_cache_path as cache_path too
      CefString(&settings.cache_path).FromASCII(root_cache_path);
      GST_INFO_OBJECT(src, "Also setting cache_path=%s", root_cache_path);
    }
  }

  // Enable CEF debug log to file
  gchar *cef_log_path = g_build_filename(root_cache_path ? root_cache_path : ".", "cef_debug.log", nullptr);
  CefString(&settings.log_file).FromASCII(cef_log_path);
  GST_INFO_OBJECT(src, "CEF log file: %s", cef_log_path);
  g_free(cef_log_path);

  app = new BrowserApp(src);

  if (!CefInitialize(args, settings, app, nullptr)) {
    GST_ERROR ("Failed to initialize CEF");

    /* unblock start () */
    g_mutex_lock (&init_lock);
    cef_status = CEF_STATUS_FAILURE;
    g_cond_broadcast (&init_cond);
    g_mutex_unlock (&init_lock);

    goto done;
  }

  g_mutex_lock (&init_lock);
  cef_status = CEF_STATUS_INITIALIZED;
  g_cond_broadcast (&init_cond);
  g_mutex_unlock (&init_lock);
#ifndef __APPLE__
  CefRunMessageLoop();
#ifdef _WIN32
  CoUninitialize();
#endif
#endif

done:
  return NULL;
}

static GstStateChangeReturn
gst_cef_src_change_state(GstElement *src, GstStateChange transition)
{
  GstStateChangeReturn result = GST_STATE_CHANGE_SUCCESS;

  switch(transition)
  {
  case GST_STATE_CHANGE_NULL_TO_READY:
  {
    g_mutex_lock (&init_lock);
    // Wait till a previous CEF is dismantled or completes initialization
    while (cef_status & CEF_STATUS_MASK_TRANSITIONING)
      g_cond_wait (&init_cond, &init_lock);
    if (cef_status == CEF_STATUS_FAILURE) {
      // BAIL OUT, CEF is not loaded.
      result = GST_STATE_CHANGE_FAILURE;
    } else if (cef_status == CEF_STATUS_NOT_LOADED) {
      cef_status = CEF_STATUS_INITIALIZING;
      /* Initialize Chromium Embedded Framework */
#ifdef __APPLE__
      /* in the main thread as per Cocoa */
      if (pthread_main_np()) {
        g_mutex_unlock (&init_lock);
        init_cef ((GstCefSrc*) src);
        g_mutex_lock (&init_lock);
      } else {
        dispatch_async_f(dispatch_get_main_queue(), (GstCefSrc*)src, (dispatch_function_t)&init_cef);
        while (cef_status == CEF_STATUS_INITIALIZING)
          g_cond_wait (&init_cond, &init_lock);
      }
#else
        /* in a separate UI thread */
      thread = g_thread_new("cef-ui-thread", (GThreadFunc) init_cef, (GstCefSrc*)src);
      while (cef_status == CEF_STATUS_INITIALIZING)
        g_cond_wait (&init_cond, &init_lock);
#endif
      if (cef_status & ~CEF_STATUS_MASK_INITIALIZED) {
        // BAIL OUT, CEF is not loaded.
        result = GST_STATE_CHANGE_FAILURE;
#ifndef __APPLE__
        g_thread_join(thread);
        thread = nullptr;
#endif
      }
    }
    g_mutex_unlock(&init_lock);

    GstCefSrc *cefsrc = GST_CEF_SRC (src);
    gst_buffer_replace (&cefsrc->current_buffer, NULL);

    break;
  }
  default:
    break;
  }

  if (result == GST_STATE_CHANGE_FAILURE) return result;
  result = GST_ELEMENT_CLASS(parent_class)->change_state(src, transition);

  return result;
}

static gboolean
gst_cef_src_start(GstBaseSrc *base_src)
{
  gboolean ret = FALSE;
  GstCefSrc *src = GST_CEF_SRC (base_src);

#ifdef GST_CEF_ENABLE_CUDA
  if (src->cuda_diagnostic_pairs) {
    gint num_buffers = -1;
    g_object_get(src, "num-buffers", &num_buffers, nullptr);
    if (!src->cuda_memory || (num_buffers > 0 && num_buffers % 2 != 0)) {
      GST_ELEMENT_ERROR(src, RESOURCE, SETTINGS,
          ("CUDA diagnostic pairs require cuda-memory=true and an even num-buffers limit"), (nullptr));
      return FALSE;
    }
    GST_WARNING_OBJECT(src, "CUDA paired-copy diagnostics enabled: delay=%u us; output is not a performance benchmark", src->cuda_pair_delay_us);
  }
  src->cuda_pair_flushing = FALSE;
  src->cuda_stopping = FALSE;
#endif

  GST_ELEMENT_PROGRESS(src, START, "open", ("Creating CEF browser client"));

  CefRefPtr<BrowserClient> browserClient = new BrowserClient(src);

    /* Make sure CEF is initialized before posting a task */
  g_mutex_lock (&init_lock);
  while (cef_status & ~CEF_STATUS_MASK_INITIALIZED)
    g_cond_wait (&init_cond, &init_lock);
  g_mutex_unlock (&init_lock);

  if (cef_status == CEF_STATUS_FAILURE) {
    GST_ELEMENT_PROGRESS(src, ERROR, "open", ("CEF in failed state (early check)"));
    goto done;
  }

  /* Wait for OnContextInitialized - message loop must be running before CreateBrowser */
  GST_INFO_OBJECT(src, "Waiting for CEF context initialization (message loop)...");
  g_mutex_lock (&init_lock);
  while (!context_initialized && cef_status != CEF_STATUS_FAILURE)
    g_cond_wait (&init_cond, &init_lock);
  g_mutex_unlock (&init_lock);
  GST_INFO_OBJECT(src, "CEF context initialized, proceeding with browser creation");

  if (cef_status == CEF_STATUS_FAILURE) {
    GST_ELEMENT_PROGRESS(src, ERROR, "open", ("CEF in failed state"));
    goto done;
  }

  GST_OBJECT_LOCK (src);
  src->n_frames = 0;
#ifdef GST_CEF_ENABLE_CUDA
  src->cuda_clock->Reset();
  src->cuda_frames->Clear();
  src->cuda_frames->CancelResume();
  src->cuda_selected_sequence = 0;
#endif
  GST_OBJECT_UNLOCK (src);

  GST_ELEMENT_PROGRESS(src, CONTINUE, "open", ("Creating CEF browser ..."));

#ifdef __APPLE__
  if (pthread_main_np()) {
    /* in the main thread as per Cocoa */
    browserClient->MakeBrowser(0);
  } else {
#endif
    CefPostTask(TID_UI, base::BindOnce(&BrowserClient::MakeBrowser, browserClient.get(), 0));

    /* And wait for this src's browser to have been created */
    GST_ELEMENT_PROGRESS(src, CONTINUE, "open", ("Waiting for CEF browser initialization..."));

    g_mutex_lock(&src->state_lock);
    while (!CefSrcStateIsOpen(src->state))
      g_cond_wait (&src->state_cond, &src->state_lock);
    g_mutex_unlock (&src->state_lock);
#ifdef __APPLE__
  }
#endif


  if (src->listen_for_js_signals) {
    g_mutex_lock (&src->state_lock);
    while (src->state == CEF_SRC_WAITING_FOR_READY)
      g_cond_wait (&src->state_cond, &src->state_lock);
    g_mutex_unlock (&src->state_lock);
  }

  g_mutex_lock (&src->state_lock);
  ret = src->browser != NULL;
  g_mutex_unlock (&src->state_lock);

  if (ret) {
    GST_ELEMENT_PROGRESS(
      src, COMPLETE, "open",
      ("CEF browser created")
    );
  } else {
    GST_ELEMENT_PROGRESS(
      src, ERROR, "open",
      ("CEF browser failed to create")
    );
  }

done:
  return ret;
}

static void
gst_cef_src_close_browser(GstCefSrc *src)
{
  CefPostTask (TID_UI,
      base::BindOnce (&gst_cef_close_browser_on_ui_thread, src->browser));
}

static gboolean
gst_cef_src_stop (GstBaseSrc *base_src)
{
  GstCefSrc *src = GST_CEF_SRC (base_src);

  GST_INFO_OBJECT (src, "Stopping");

#ifdef GST_CEF_ENABLE_CUDA
  GST_OBJECT_LOCK(src);
  src->cuda_stopping = TRUE;
  src->cuda_clock->Cancel();
  src->cuda_frames->CancelResume();
  src->cuda_frames->Clear();
  GST_OBJECT_UNLOCK(src);
#endif

  if (src->browser) {
    gst_cef_src_close_browser(src);
#ifdef __APPLE__
    if (!pthread_main_np()) {
#endif
      /* And wait for this src's browser to have been closed */
      g_mutex_lock(&src->state_lock);
      while (CefSrcStateIsOpen(src->state))
        g_cond_wait (&src->state_cond, &src->state_lock);
      g_mutex_unlock (&src->state_lock);
#ifdef __APPLE__
    }
#endif
  }

  GST_OBJECT_LOCK(src);
  gst_buffer_replace (&src->current_buffer, NULL);
#ifdef GST_CEF_ENABLE_CUDA
  if (src->cuda_pairs) src->cuda_pairs->Clear();
  src->cuda_popup_visible = FALSE;
  src->cuda_popup_x = src->cuda_popup_y = src->cuda_popup_width = src->cuda_popup_height = 0;
  delete src->cuda_frame;
  src->cuda_frame = nullptr;
  src->cuda_failed = FALSE;
#endif
  GST_OBJECT_UNLOCK(src);

  return TRUE;
}

static void
gst_cef_src_get_times (GstBaseSrc * base_src, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
#ifdef GST_CEF_ENABLE_CUDA
  GstCefSrc* src = GST_CEF_SRC(base_src);
  if (src->cuda_memory && !src->cuda_diagnostic_pairs) {
    *start = *end = GST_CLOCK_TIME_NONE;
    return;
  }
#endif
  GstClockTime timestamp = GST_BUFFER_PTS (buffer);
  GstClockTime duration = GST_BUFFER_DURATION (buffer);

  *end = timestamp + duration;
  *start = timestamp;

  GST_LOG_OBJECT (base_src, "Got times start: %" GST_TIME_FORMAT " end: %" GST_TIME_FORMAT, GST_TIME_ARGS (*start), GST_TIME_ARGS (*end));
}

static gboolean
gst_cef_src_query (GstBaseSrc * base_src, GstQuery * query)
{
  gboolean res = FALSE;
  GstCefSrc *src = GST_CEF_SRC (base_src);

  switch (GST_QUERY_TYPE (query)) {
#ifdef GST_CEF_ENABLE_CUDA
    case GST_QUERY_CONTEXT:
      if (gst_cuda_handle_context_query(GST_ELEMENT(src), query, src->cuda_context)) return TRUE;
      return GST_BASE_SRC_CLASS(parent_class)->query(base_src, query);
#endif
    case GST_QUERY_LATENCY:
    {
      GstClockTime latency;

      if (src->vinfo.fps_n) {
        latency = gst_util_uint64_scale (GST_SECOND, src->vinfo.fps_d, src->vinfo.fps_n);
        GST_DEBUG_OBJECT (src, "Reporting latency: %" GST_TIME_FORMAT, GST_TIME_ARGS (latency));
        gst_query_set_latency (query, TRUE, latency, GST_CLOCK_TIME_NONE);
      }
      res = TRUE;
      break;
    }
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (base_src, query);
      break;
  }

  return res;
}

#ifdef GST_CEF_ENABLE_CUDA
static gboolean gst_cef_src_set_clock(GstElement* element, GstClock* clock) {
  const gboolean accepted = GST_ELEMENT_CLASS(parent_class)->set_clock(element, clock);
  GstCefSrc* src = GST_CEF_SRC(element);
  if (accepted && src->cuda_memory && !src->cuda_diagnostic_pairs) src->cuda_clock->Invalidate();
  return accepted;
}

static gboolean gst_cef_src_unlock(GstBaseSrc* base) {
  GstCefSrc* src = GST_CEF_SRC(base);
  if (!src->cuda_memory) return TRUE;
  GST_OBJECT_LOCK(src);
  src->cuda_pair_flushing = TRUE;
  src->cuda_clock->Cancel();
  if (!src->cuda_diagnostic_pairs) src->cuda_frames->BeginFlush();
  else src->cuda_frames->Clear();
  if (src->cuda_pairs) src->cuda_pairs->Clear();
  g_cond_broadcast(&src->cuda_pair_cond);
  GST_OBJECT_UNLOCK(src);
  return TRUE;
}

static gboolean gst_cef_src_unlock_stop(GstBaseSrc* base) {
  GstCefSrc* src = GST_CEF_SRC(base);
  if (!src->cuda_memory) return TRUE;
  g_mutex_lock(&src->state_lock);
  CefRefPtr<CefBrowser> browser = src->browser;
  g_mutex_unlock(&src->state_lock);
  GST_OBJECT_LOCK(src);
  src->cuda_pair_flushing = FALSE;
  src->cuda_clock->Resume();
  const guint64 generation = src->cuda_frames->ResumeGeneration();
  const bool refresh = !src->cuda_diagnostic_pairs && browser && !src->cuda_stopping &&
      src->cuda_frames->ResumePending();
  if (!browser) src->cuda_frames->CancelResume();
  GST_OBJECT_UNLOCK(src);
  if (refresh) {
    // Retain both objects until the task completes or CEF rejects the task.
    auto retained = std::shared_ptr<GstCefSrc>(GST_CEF_SRC(gst_object_ref(src)),
        [](GstCefSrc* source) { gst_object_unref(source); });
    if (!CefPostTask(TID_UI, base::BindOnce([](std::shared_ptr<GstCefSrc> retained, CefRefPtr<CefBrowser> browser, guint64 generation) {
      GstCefSrc* source = retained.get();
      GST_OBJECT_LOCK(source);
      if (source->browser != browser || source->cuda_failed ||
          !source->cuda_frames->CanResume(generation, source->cuda_pair_flushing, source->cuda_stopping)) {
        GST_OBJECT_UNLOCK(source); return;
      }
      std::string error;
      gst_cef_src_apply_popup_locked(source, error);
      source->cuda_frames->FinishResume();
      const bool popup_visible = source->cuda_popup_visible;
      if (!error.empty()) source->cuda_failed = TRUE;
      GST_OBJECT_UNLOCK(source);
      if (!error.empty()) {
        GST_ELEMENT_ERROR(source, RESOURCE, FAILED, ("CUDA popup resume failed"), ("%s", error.c_str()));
        return;
      }
      browser->GetHost()->Invalidate(PET_VIEW);
      if (popup_visible) browser->GetHost()->Invalidate(PET_POPUP);
    }, retained, browser, generation))) {
      GST_ELEMENT_ERROR(src, RESOURCE, FAILED, ("Cannot schedule CUDA browser resume"), (nullptr));
      return FALSE;
    }
  }
  return TRUE;
}

static void gst_cef_src_set_context(GstElement* element, GstContext* context) {
  GstCefSrc* src = GST_CEF_SRC(element);
  GST_OBJECT_LOCK(src);
  gst_cuda_handle_set_context(element, context, -1, &src->cuda_context);
  GST_OBJECT_UNLOCK(src);
  GST_ELEMENT_CLASS(parent_class)->set_context(element, context);
}

static GstCaps* gst_cef_src_get_caps(GstBaseSrc* base, GstCaps* filter) {
  GstCefSrc* src = GST_CEF_SRC(base);
  GstCaps* caps = gst_caps_from_string(src->cuda_memory ? CEF_CUDA_VIDEO_CAPS : CEF_SYSTEM_VIDEO_CAPS);
  if (filter) {
    GstCaps* result = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(caps); return result;
  }
  return caps;
}
#endif

static GstCaps *
gst_cef_src_fixate (GstBaseSrc * base_src, GstCaps * caps)
{
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);
  structure = gst_caps_get_structure (caps, 0);

  gst_structure_fixate_field_nearest_int (structure, "width", DEFAULT_WIDTH);
  gst_structure_fixate_field_nearest_int (structure, "height", DEFAULT_HEIGHT);

  if (gst_structure_has_field (structure, "framerate"))
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", DEFAULT_FPS_N, DEFAULT_FPS_D);
  else
    gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, DEFAULT_FPS_N, DEFAULT_FPS_D, nullptr);


  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (base_src, caps);

  GST_INFO_OBJECT (base_src, "Fixated caps to %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
gst_cef_src_set_caps (GstBaseSrc * base_src, GstCaps * caps)
{
  GstCefSrc *src = GST_CEF_SRC (base_src);
  gboolean ret = TRUE;
  GstBuffer *new_buffer;

  GST_INFO_OBJECT (base_src, "Caps set to %" GST_PTR_FORMAT, caps);

  GstVideoInfo negotiated;
  if (!gst_video_info_from_caps(&negotiated, caps)) return FALSE;
#ifdef GST_CEF_ENABLE_CUDA
  bool cuda_caps = gst_caps_features_contains(gst_caps_get_features(caps, 0), GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY);
  if (cuda_caps != bool(src->cuda_memory)) {
    GST_ERROR_OBJECT(src, "cuda-memory must match negotiated CUDA caps"); return FALSE;
  }
  // Context discovery can call set_context; do not hold the object lock here.
  if (cuda_caps && (!gst_cuda_load_library() ||
      !gst_cuda_ensure_element_context(GST_ELEMENT(src), -1, &src->cuda_context))) return FALSE;
#endif
  GST_OBJECT_LOCK (src);
  src->vinfo = negotiated;
#ifdef GST_CEF_ENABLE_CUDA
  if (cuda_caps) {
    if (src->cuda_diagnostic_pairs) {
      if (src->n_frames != 0) {
        GST_OBJECT_UNLOCK(src);
        GST_ELEMENT_ERROR(src, RESOURCE, SETTINGS, ("Paired diagnostics do not support mid-capture caps changes"), (nullptr));
        return FALSE;
      }
      if (!src->cuda_pairs) src->cuda_pairs = new GpuPairQueue();
      src->cuda_pairs->Clear();
    }
    src->cuda_frames->Reset(&src->current_buffer, &src->cuda_selected_sequence, nullptr, 0);
    delete src->cuda_frame;
    src->cuda_frame = new LinuxCudaFrame(src->cuda_context, src->vinfo, caps);
    std::string error;
    if (src->cuda_popup_visible) {
      CefRect bounds(src->cuda_popup_x, src->cuda_popup_y, src->cuda_popup_width, src->cuda_popup_height);
      src->cuda_frame->UpdatePopup(true, bounds, error);
    }
    new_buffer = src->cuda_frame->Blank(error);
    src->cuda_failed = !new_buffer;
    if (!new_buffer) {
      GST_OBJECT_UNLOCK(src);
      GST_ELEMENT_ERROR(src, RESOURCE, FAILED, ("Cannot configure CUDA browser output"), ("%s", error.c_str()));
      return FALSE;
    }
  } else
#endif
  {
    new_buffer = gst_buffer_new_allocate (NULL, src->vinfo.size, NULL);
    gst_buffer_memset(new_buffer, 0, 0, src->vinfo.size);
  }
  gst_buffer_replace (&(src->current_buffer), new_buffer);
  gst_buffer_unref (new_buffer);
  if (src->browser) {
    CefRefPtr<CefBrowser> browser = src->browser;
    int fps = (int)gst_util_uint64_scale (1, src->vinfo.fps_n, src->vinfo.fps_d);
    if (src->paint_rate > 0 && src->paint_rate < fps) {
      GST_INFO_OBJECT (src, "Capping browser paint rate to %d (stream fps %d)",
          src->paint_rate, fps);
      fps = src->paint_rate;
    }
    CefPostTask (TID_UI,
        base::BindOnce ([](CefRefPtr<CefBrowser> b, int rate) {
          CEF_REQUIRE_UI_THREAD();
          if (b && b->GetHost()) {
            b->GetHost()->SetWindowlessFrameRate(rate);
            b->GetHost()->WasResized();
            b->GetHost()->Invalidate(PET_VIEW);
          }
        }, browser, fps));
  }
  GST_OBJECT_UNLOCK (src);

  return ret;
}

static void
gst_cef_src_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstCefSrc *src = GST_CEF_SRC (object);

  switch (prop_id) {
    case PROP_URL:
    {
      const gchar *url;

      url = g_value_get_string (value);
      g_free (src->url);
      src->url = g_strdup (url);

      g_mutex_lock(&src->state_lock);
      if (CefSrcStateIsOpen(src->state) && src->browser) {
        CefPostTask (TID_UI,
            base::BindOnce (&gst_cef_load_url_on_ui_thread,
                src->browser,
                std::string (src->url)));
      }
      g_mutex_unlock(&src->state_lock);

      break;
    }
    case PROP_CHROME_EXTRA_FLAGS: {
      GST_WARNING_OBJECT(
        src,
        "cefsrc chrome-extra-flags property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_CHROME_EXTRA_FLAGS instead"
      );
      g_free (src->chrome_extra_flags);
      src->chrome_extra_flags = g_value_dup_string (value);
      break;
    }
#ifdef GST_CEF_ENABLE_CUDA
    case PROP_CUDA_DIAGNOSTIC_PAIRS:
    case PROP_CUDA_PAIR_DELAY_US:
      if (GST_STATE(src) != GST_STATE_NULL) {
        GST_WARNING_OBJECT(src, "CUDA diagnostics can only change in NULL state"); break;
      }
      if (prop_id == PROP_CUDA_DIAGNOSTIC_PAIRS) src->cuda_diagnostic_pairs = g_value_get_boolean(value);
      else src->cuda_pair_delay_us = g_value_get_uint(value);
      break;
    case PROP_CUDA_MEMORY:
      if (GST_STATE(src) != GST_STATE_NULL) {
        GST_WARNING_OBJECT(src, "cuda-memory can only change in NULL state"); break;
      }
      src->cuda_memory = g_value_get_boolean(value);
      break;
#endif
    case PROP_GPU:
    {
      GST_WARNING_OBJECT(
        src,
        "cefsrc gpu property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_GPU_ENABLED instead"
      );
      src->gpu = g_value_get_boolean (value);
      break;
    }
    case PROP_CHROMIUM_DEBUG_PORT:
    {
      GST_WARNING_OBJECT(
        src,
        "cefsrc chromium-debug-port property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_CHROME_EXTRA_FLAGS instead"
      );
      src->chromium_debug_port = g_value_get_int (value);
      break;
    }
    case PROP_PAINT_RATE:
      src->paint_rate = g_value_get_int (value);
      break;
    case PROP_SANDBOX:
    {
      GST_WARNING_OBJECT(
        src,
        "cefsrc sandbox property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_SANDBOX instead"
      );
      src->sandbox = g_value_get_boolean (value);
      break;
    }
    case PROP_LISTEN_FOR_JS_SIGNAL:
    {
      src->listen_for_js_signals = g_value_get_boolean (value);
      break;
    }
    case PROP_JS_FLAGS:
    {
      GST_WARNING_OBJECT(
        src,
        "cefsrc js-flags property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_JS_FLAGS instead"
      );
      g_free (src->js_flags);
      src->js_flags = g_value_dup_string (value);
      break;
    }
    case PROP_LOG_SEVERITY:
    {
      GST_WARNING_OBJECT(
        src,
        "cefsrc log-severity property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_LOG_SEVERITY instead"
      );
      src->log_severity = (cef_log_severity_t) g_value_get_enum (value);
      break;
    }
    case PROP_CEF_CACHE_LOCATION:
    {
      GST_WARNING_OBJECT(
        src,
        "cefsrc cef-cache-location property is deprecated and is global across all cefsrc instances - "
        "set GST_CEF_CACHE_LOCATION instead"
      );
      g_free (src->cef_cache_location);
      src->cef_cache_location = g_value_dup_string (value);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cef_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstCefSrc *src = GST_CEF_SRC (object);

  switch (prop_id) {
    case PROP_URL:
      g_value_set_string (value, src->url);
      break;
    case PROP_CHROME_EXTRA_FLAGS:
      g_value_set_string (value, src->chrome_extra_flags);
      break;
#ifdef GST_CEF_ENABLE_CUDA
    case PROP_CUDA_DIAGNOSTIC_PAIRS:
      g_value_set_boolean(value, src->cuda_diagnostic_pairs);
      break;
    case PROP_CUDA_PAIR_DELAY_US:
      g_value_set_uint(value, src->cuda_pair_delay_us);
      break;
    case PROP_CUDA_MEMORY:
      g_value_set_boolean(value, src->cuda_memory);
      break;
#endif
    case PROP_GPU:
      g_value_set_boolean (value, src->gpu);
      break;
    case PROP_CHROMIUM_DEBUG_PORT:
      g_value_set_int (value, src->chromium_debug_port);
      break;
    case PROP_PAINT_RATE:
      g_value_set_int (value, src->paint_rate);
      break;
    case PROP_SANDBOX:
      g_value_set_boolean (value, src->sandbox);
      break;
    case PROP_LISTEN_FOR_JS_SIGNAL:
      g_value_set_boolean (value, src->listen_for_js_signals);
      break;
    case PROP_JS_FLAGS:
      g_value_set_string (value, src->js_flags);
      break;
    case PROP_LOG_SEVERITY:
      g_value_set_enum (value, src->log_severity);
      break;
    case PROP_CEF_CACHE_LOCATION:
      g_value_set_string (value, src->cef_cache_location);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cef_src_finalize (GObject *object)
{
  GstCefSrc *src = GST_CEF_SRC (object);

#ifdef _WIN32
  if (src->texture_reader) {
    delete src->texture_reader;
    src->texture_reader = nullptr;
  }
#endif

#ifdef GST_CEF_ENABLE_CUDA
  gst_buffer_replace(&src->current_buffer, nullptr);
  delete src->cuda_pairs;
  src->cuda_pairs = nullptr;
  delete src->cuda_frames;
  src->cuda_frames = nullptr;
  delete src->cuda_clock;
  src->cuda_clock = nullptr;
  delete src->cuda_frame;
  src->cuda_frame = nullptr;
  gst_clear_object(&src->cuda_context);
  g_cond_clear(&src->cuda_pair_cond);
#endif
  if (src->audio_buffers) {
    gst_buffer_list_unref (src->audio_buffers);
    src->audio_buffers = NULL;
  }

  g_list_free_full (src->audio_events, (GDestroyNotify) gst_event_unref);
  src->audio_events = NULL;

  g_free (src->js_flags);
  g_free (src->cef_cache_location);

  g_cond_clear(&src->state_cond);
  g_mutex_clear(&src->state_lock);
}

static void
gst_cef_src_init (GstCefSrc * src)
{
  GstBaseSrc *base_src = GST_BASE_SRC (src);

  src->n_frames = 0;
#ifdef GST_CEF_ENABLE_CUDA
  src->cuda_memory = FALSE;
  src->cuda_publish_sequence = 0;
  src->cuda_selected_sequence = 0;
  src->cuda_frames = new GpuFrameQueue();
  src->cuda_clock = new GpuFrameClock();
  src->cuda_failed = FALSE;
  src->cuda_stopping = FALSE;
  src->cuda_popup_visible = FALSE;
  src->cuda_popup_x = src->cuda_popup_y = src->cuda_popup_width = src->cuda_popup_height = 0;
  src->cuda_diagnostic_pairs = FALSE;
  src->cuda_pair_flushing = FALSE;
  src->cuda_pair_delay_us = 1000;
  src->cuda_pairs = nullptr;
  g_cond_init(&src->cuda_pair_cond);
  src->cuda_context = nullptr;
  src->cuda_frame = nullptr;
#endif
  src->current_buffer = NULL;
  src->audio_buffers = NULL;
  src->audio_events = NULL;
  src->state = CEF_SRC_CLOSED;
#ifdef _WIN32
  src->texture_reader = nullptr;
#endif
  src->chromium_debug_port = DEFAULT_CHROMIUM_DEBUG_PORT;
  src->paint_rate = 0;

  /* Default video info so GetViewRect returns sensible values before caps */
  gst_video_info_set_format(&src->vinfo, GST_VIDEO_FORMAT_BGRA, DEFAULT_WIDTH, DEFAULT_HEIGHT);
  src->vinfo.fps_n = DEFAULT_FPS_N;
  src->vinfo.fps_d = DEFAULT_FPS_D;
  src->sandbox = DEFAULT_SANDBOX;
  src->listen_for_js_signals = DEFAULT_LISTEN_FOR_JS_SIGNALS;
  src->js_flags = NULL;
  src->log_severity = DEFAULT_LOG_SEVERITY;
  src->cef_cache_location = NULL;

  gst_base_src_set_format (base_src, GST_FORMAT_TIME);
  gst_base_src_set_live (base_src, TRUE);

  g_cond_init (&src->state_cond);
  g_mutex_init (&src->state_lock);
}

static void
gst_cef_src_class_init (GstCefSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);

  gobject_class->set_property = gst_cef_src_set_property;
  gobject_class->get_property = gst_cef_src_get_property;
  gobject_class->finalize = gst_cef_src_finalize;

  g_object_class_install_property (gobject_class, PROP_URL,
      g_param_spec_string ("url", "url",
          "The URL to display",
          DEFAULT_URL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

#ifdef GST_CEF_ENABLE_CUDA
  g_object_class_install_property(gobject_class, PROP_CUDA_MEMORY,
      g_param_spec_boolean("cuda-memory", "CUDA memory", "Experimental owned Linux CUDA output; set before READY",
          FALSE, GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(gobject_class, PROP_CUDA_DIAGNOSTIC_PAIRS,
      g_param_spec_boolean("cuda-diagnostic-pairs", "CUDA diagnostic pairs",
          "Test only: emit two independent copies per sampled callback; excludes synthetic repeats and startup blank frames",
          FALSE, GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(gobject_class, PROP_CUDA_PAIR_DELAY_US,
      g_param_spec_uint("cuda-pair-delay-us", "CUDA pair delay",
          "Test only: hold the CEF callback between diagnostic copies, in microseconds",
          0, 100000, 1000, GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  gstelement_class->set_clock = GST_DEBUG_FUNCPTR(gst_cef_src_set_clock);
  base_src_class->unlock = GST_DEBUG_FUNCPTR(gst_cef_src_unlock);
  base_src_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_cef_src_unlock_stop);
  gstelement_class->set_context = GST_DEBUG_FUNCPTR(gst_cef_src_set_context);
  base_src_class->get_caps = GST_DEBUG_FUNCPTR(gst_cef_src_get_caps);
#endif

  g_object_class_install_property (gobject_class, PROP_GPU,
    g_param_spec_boolean ("gpu", "gpu",
          "Enable GPU usage in chromium (Improves performance if you have GPU) - "
          "deprecated: set GST_CEF_GPU_ENABLED in the environment instead",
          DEFAULT_GPU, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CHROMIUM_DEBUG_PORT,
    g_param_spec_int ("chromium-debug-port", "chromium-debug-port",
          "Set chromium debug port (-1 = disabled) - "
          "deprecated: set GST_CEF_CHROME_EXTRA_FLAGS in the environment instead", -1, G_MAXUINT16,
          DEFAULT_CHROMIUM_DEBUG_PORT, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_PAINT_RATE,
    g_param_spec_int ("paint-rate", "paint-rate",
          "Cap the browser paint/composite rate (fps) independently of the "
          "negotiated stream framerate (0 = follow the stream framerate). "
          "The source duplicates the last painted frame up to the stream rate.",
          0, 240, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CHROME_EXTRA_FLAGS,
    g_param_spec_string ("chrome-extra-flags", "chrome-extra-flags",
          "Comma delimiter flags to be passed into chrome "
          "(Example: show-fps-counter,remote-debugging-port=9222) - "
          "deprecated: set GST_CEF_CHROME_EXTRA_FLAGS in the environment instead",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_SANDBOX,
    g_param_spec_boolean ("sandbox", "sandbox",
          "Toggle chromium sandboxing capabilities - "
          "deprecated: set GST_CEF_SANDBOX in the environment instead",
          DEFAULT_SANDBOX, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_LISTEN_FOR_JS_SIGNAL,
    g_param_spec_boolean ("listen-for-js-signals", "listen-for-js-signals",
          "Listen and respond to signals sent from javascript: "
          "window.gstSendMsg({request: \"ready|eos\", ...}) - "
          "see [README](https://github.com/centricular/gstcefsrc?tab=readme-ov-file#javascript-signals) "
          "for more detail",
          DEFAULT_LISTEN_FOR_JS_SIGNALS, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_JS_FLAGS,
    g_param_spec_string ("js-flags", "js-flags",
          "Space delimited JavaScript flags to be passed to Chromium "
          "(Example: --noexpose_wasm --expose-gc) - "
          "deprecated: set GST_CEF_JS_FLAGS in the environment instead",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_LOG_SEVERITY,
      g_param_spec_enum ("log-severity", "log-severity",
          "CEF log severity level - "
          "deprecated: set GST_CEF_LOG_SEVERITY in the environment instead",
          GST_TYPE_CEF_LOG_SEVERITY_MODE, DEFAULT_LOG_SEVERITY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CEF_CACHE_LOCATION,
    g_param_spec_string ("cef-cache-location", "cef-cache-location",
          "Cache location for CEF. Defaults to in memory cache. "
          "(Example: /tmp/cef-cache/) - "
          "deprecated: set GST_CEF_CACHE_LOCATION in the environment instead",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  gst_element_class_set_static_metadata (gstelement_class,
      "Chromium Embedded Framework source", "Source/Video",
      "Creates a video stream from an embedded Chromium browser",
      "Mathieu Duponchelle <mathieu@centricular.com>");

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_cef_src_template);

  base_src_class->fixate = GST_DEBUG_FUNCPTR(gst_cef_src_fixate);
  base_src_class->set_caps = GST_DEBUG_FUNCPTR(gst_cef_src_set_caps);
  base_src_class->start = GST_DEBUG_FUNCPTR(gst_cef_src_start);
  base_src_class->stop = GST_DEBUG_FUNCPTR(gst_cef_src_stop);
  base_src_class->get_times = GST_DEBUG_FUNCPTR(gst_cef_src_get_times);
  base_src_class->query = GST_DEBUG_FUNCPTR(gst_cef_src_query);

  gstelement_class->change_state = GST_DEBUG_FUNCPTR(gst_cef_src_change_state);

  push_src_class->create = GST_DEBUG_FUNCPTR(gst_cef_src_create);

  GST_DEBUG_CATEGORY_INIT (cef_src_debug, "cefsrc", 0,
      "Chromium Embedded Framework Source");
#ifdef GST_CEF_ENABLE_CUDA
  GST_DEBUG_CATEGORY_INIT (cef_cadence_debug, "cefcadence", 0,
      "CUDA browser publication and selection timing");
#endif
  GST_DEBUG_CATEGORY_INIT (cef_console_debug, "cefconsole", 0,
      "Chromium Embedded Framework JS Console");
}