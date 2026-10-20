#include <cstdio>
#include <glib.h>
#include <sstream>
#include <string>
#include <mutex>

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
#include "gstcefalpha.h"
#include "gstceftag.h"
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

GST_DEBUG_CATEGORY_STATIC (cef_lock_debug);

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
#define DEFAULT_FRAME_LOCK FALSE
#define DEFAULT_FRAME_LOCK_LATENCY (100 * GST_MSECOND)
#define DEFAULT_FRAME_LOCK_TIMEOUT (40 * GST_MSECOND)
/* Requests beyond this depth mean the browser cannot keep up; the oldest
 * request is dropped. Video reaches the tap in bursts of about 20 frames,
 * so the queue holds several bursts. */
#define FRAME_LOCK_MAX_PENDING 64
/* Interval between BeginFrame re-sends while a locked frame waits for paint. */
#define FRAME_LOCK_RESEND_US 2000
#define FRAME_TAG_RING 16
/* A repeat comes from the paint ring, so the request times must reach as far. */
G_STATIC_ASSERT (FRAME_TAG_TIMES >= FRAME_TAG_RING);
/* Video frames without a readable tag, while paints keep coming, before the
 * newest paint goes out unpaired (a graphic over the tag). */
#define FRAME_TAG_FALLBACK_FRAMES 3

typedef struct {
  GstClockTime running_time;
  gint64 tag; /* -1 for an untagged (frame-lock) request */
} CefFrameRequest;

static gboolean
gst_cef_tag_not_before (gint64 a, gint64 b)
{
  return ((a - b) & FRAME_TAG_MASK) < 0x800000;
}

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
  PROP_CHROMIUM_DEBUG_PORT,
  PROP_PAINT_RATE,
  PROP_CHROME_EXTRA_FLAGS,
  PROP_SANDBOX,
  PROP_LISTEN_FOR_JS_SIGNAL,
  PROP_JS_FLAGS,
  PROP_LOG_SEVERITY,
  PROP_CEF_CACHE_LOCATION,
  PROP_FRAME_LOCK,
  PROP_FRAME_LOCK_LATENCY,
  PROP_FRAME_LOCK_TIMEOUT,
  PROP_FRAME_TAG,
};

enum
{
  SIGNAL_REQUEST_FRAME,
  SIGNAL_REQUEST_TAGGED_FRAME,
  LAST_SIGNAL,
};

static guint gst_cef_src_signals[LAST_SIGNAL] = { 0 };

#define gst_cef_src_parent_class parent_class
G_DEFINE_TYPE (GstCefSrc, gst_cef_src, GST_TYPE_PUSH_SRC);

#define CEF_VIDEO_CAPS "video/x-raw, format=BGRA, width=[1, 2147483647], height=[1, 2147483647], framerate=[1/1, 60/1], pixel-aspect-ratio=1/1"
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
      {
        GstMapInfo map;
        if (!gst_buffer_map (new_buffer, &map, GST_MAP_WRITE)) {
          GST_ERROR_OBJECT (src, "Failed to map GstBuffer for CPU frame");
          gst_buffer_unref (new_buffer);
          return;
        }
        /* Windows feeds the desktop app (NDI). Its output stays premultiplied
         * until NDI receivers are checked; see OnAcceleratedPaint. */
#ifdef _WIN32
        memcpy (map.data, buffer, copy_size);
#else
        gst_cef_unpremultiply_bgra (map.data, (const uint8_t *) buffer, copy_size / 4);
#endif
        if (copy_size < target_size)
          memset (map.data + copy_size, 0, target_size - copy_size);
        gst_buffer_unmap (new_buffer, &map);
      }

      gint64 tag = -1, squeeze = -1;
      if (src->frame_tag && type == PET_VIEW && (guint) w == target_width && (guint) h == target_height) {
        tag = gst_cef_read_frame_tag ((const guint8 *) buffer, w, h);
        if (tag >= 0) {
          /* The tags are not part of the picture: make them transparent. */
          gst_buffer_memset (new_buffer,
              ((gsize) (h - 1) * w + (w - FRAME_TAG_PIXELS)) * 4, 0, FRAME_TAG_PIXELS * 4);
          squeeze = gst_cef_read_squeeze_tag ((const guint8 *) buffer, w, h);
          if (squeeze >= 0)
            gst_buffer_memset (new_buffer, (gsize) (h - 1) * w * 4, 0, FRAME_TAG_PIXELS * 4);
        }
      }

      GST_OBJECT_LOCK (src);
      gst_buffer_replace (&(src->current_buffer), new_buffer);
      if (src->frame_tag && type == PET_VIEW) {
        gint64 painted_at = g_get_monotonic_time ();
        if (tag < 0) {
          src->ft_untagged++;
          src->last_untagged_us = painted_at;
          /* A waiting request may give up on tags (FRAME_TAG_FALLBACK_FRAMES). */
          g_cond_broadcast (&src->frame_cond);
        } else {
          src->last_tagged_us = painted_at;
          guint slot;
          if (src->tag_count < FRAME_TAG_RING) {
            slot = src->tag_count++;
          } else {
            gst_buffer_unref (src->tag_bufs[0]);
            memmove (&src->tag_bufs[0], &src->tag_bufs[1], sizeof (GstBuffer *) * (FRAME_TAG_RING - 1));
            memmove (&src->tag_ids[0], &src->tag_ids[1], sizeof (gint64) * (FRAME_TAG_RING - 1));
            memmove (&src->tag_squeeze[0], &src->tag_squeeze[1], sizeof (gint64) * (FRAME_TAG_RING - 1));
            slot = FRAME_TAG_RING - 1;
          }
          src->tag_bufs[slot] = gst_buffer_ref (new_buffer);
          src->tag_ids[slot] = tag;
          src->tag_squeeze[slot] = squeeze;
          src->paint_seq++;
          g_cond_broadcast (&src->frame_cond);
        }
      }
      gst_buffer_unref (new_buffer);
      if (type == PET_VIEW) {
        /* Paint rate, logged once a second (ceflock INFO). */
        gint64 now = g_get_monotonic_time ();
        src->fl_paints_window++;
        if (now - src->fl_paints_window_start >= G_USEC_PER_SEC) {
          if (src->fl_paints_window_start)
            GST_CAT_INFO_OBJECT (cef_lock_debug, src, "paints/s %.1f",
                src->fl_paints_window * (gdouble) G_USEC_PER_SEC / (now - src->fl_paints_window_start));
          src->fl_paints_window = 0;
          src->fl_paints_window_start = now;
        }
      }
      if (src->frame_lock && type == PET_VIEW) {
        src->paint_seq++;
        if (src->fl_begin_frame_at) {
          gint64 us = g_get_monotonic_time () - src->fl_begin_frame_at;
          src->fl_paint_us_sum += us;
          if (us > src->fl_paint_us_max)
            src->fl_paint_us_max = us;
          src->fl_begin_frame_at = 0;
        }
        g_cond_broadcast (&src->frame_cond);
      }
      GST_OBJECT_UNLOCK (src);

      GST_LOG_OBJECT (src, "done painting");
    }

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
      window_info.external_begin_frame_enabled = src->frame_lock;
      if (src->frame_lock)
        GST_INFO_OBJECT(src, "Frame lock: external BeginFrame enabled");

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
    command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
    command_line->AppendSwitch("enable-media-stream");
    command_line->AppendSwitch("disable-dev-shm-usage"); /* https://github.com/GoogleChrome/puppeteer/issues/1834 */
    command_line->AppendSwitch("enable-begin-frame-scheduling"); /* https://bitbucket.org/chromiumembedded/cef/issues/1368 */
    if (src->frame_lock) {
      /* Make each external BeginFrame run main frame, commit, raster and draw
       * before it produces a frame, so the paint for a request reflects the
       * page state at the time of the request (the headless deterministic
       * BeginFrame setup). */
      command_line->AppendSwitch("run-all-compositor-stages-before-draw");
      command_line->AppendSwitch("disable-threaded-animation");
      command_line->AppendSwitch("disable-threaded-scrolling");
      command_line->AppendSwitch("disable-checker-imaging");
    }

    bool gpu = src->gpu || (!!g_getenv ("GST_CEF_GPU_ENABLED"));

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
}


/** cefsrc (Gstreamer) methods */

static void
gst_cef_send_begin_frame_on_ui_thread (CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();
  if (browser && browser->GetHost())
    browser->GetHost()->SendExternalBeginFrame();
}

/* Called with the object lock held. Waits for a frame request, issues one
 * external BeginFrame and waits for its paint. Returns the requested running
 * time, or GST_CLOCK_TIME_NONE when flushing. */
static GstClockTime
gst_cef_src_wait_locked_frame (GstCefSrc *src)
{
  GMutex *lock = GST_OBJECT_GET_LOCK (src);
  CefFrameRequest *req;
  GstClockTime running_time;
  guint64 seq;
  gint64 deadline;

  while (g_queue_is_empty (src->frame_requests) && !src->flushing)
    g_cond_wait (&src->frame_cond, lock);
  if (src->flushing)
    return GST_CLOCK_TIME_NONE;

  req = (CefFrameRequest *) g_queue_pop_head (src->frame_requests);
  running_time = req->running_time;
  g_free (req);

  seq = src->paint_seq;
  src->fl_begin_frame_at = g_get_monotonic_time ();
  deadline = src->fl_begin_frame_at + (gint64) (src->frame_lock_timeout / GST_USECOND);

  /* CEF ignores SendExternalBeginFrame while the previous BeginFrame has not
   * completed (begin_frame_pending_), and that completion can arrive after
   * the previous paint. Re-send until this frame paints; a re-send that CEF
   * ignores has no effect. */
  while (src->paint_seq == seq && !src->flushing) {
    gint64 now = g_get_monotonic_time ();
    if (now >= deadline)
      break;
    if (src->browser)
      CefPostTask (TID_UI, base::BindOnce (&gst_cef_send_begin_frame_on_ui_thread, src->browser));
    g_cond_wait_until (&src->frame_cond, lock, MIN (now + FRAME_LOCK_RESEND_US, deadline));
  }
  if (src->flushing)
    return GST_CLOCK_TIME_NONE;

  src->fl_frames++;
  if (src->paint_seq == seq) {
    /* No paint: the page had nothing new to draw, or the browser is late.
     * Repeat the last frame so the compositor still gets a buffer. */
    src->fl_timeouts++;
    src->fl_begin_frame_at = 0;
    GST_CAT_DEBUG_OBJECT (cef_lock_debug, src, "rt %" GST_TIME_FORMAT " no paint within timeout",
        GST_TIME_ARGS (running_time));
  }

  if (src->fl_frames % 250 == 0) {
    GST_CAT_INFO_OBJECT (cef_lock_debug, src,
        "frames %" G_GUINT64_FORMAT " timeouts %" G_GUINT64_FORMAT
        " dropped requests %" G_GUINT64_FORMAT " paint mean %.2f ms max %.2f ms",
        src->fl_frames, src->fl_timeouts, src->fl_dropped_requests,
        src->fl_frames > src->fl_timeouts
            ? src->fl_paint_us_sum / 1000.0 / (src->fl_frames - src->fl_timeouts) : 0.0,
        src->fl_paint_us_max / 1000.0);
    src->fl_paint_us_max = 0;
  }

  return running_time;
}

/* Called with the object lock held. The page still paints, but no tag has
 * read for FRAME_TAG_FALLBACK_FRAMES video frames. */
static gboolean
gst_cef_src_tags_lost (GstCefSrc *src)
{
  if (src->last_untagged_us <= src->last_tagged_us || src->vinfo.fps_n <= 0)
    return FALSE;
  gint64 frame_us = (gint64) gst_util_uint64_scale (G_USEC_PER_SEC, src->vinfo.fps_d, src->vinfo.fps_n);
  return g_get_monotonic_time () - src->last_tagged_us > FRAME_TAG_FALLBACK_FRAMES * frame_us;
}

/* Called with the object lock held. Waits for a tagged request and for the
 * paint carrying its tag. Sets *out to a new ref of the chosen paint (or NULL
 * when none exists yet), *paint_tag to that paint's tag and *squeeze_tag to
 * its squeeze tag (-1 with none).
 * Returns the requested running time, or GST_CLOCK_TIME_NONE when flushing. */
static GstClockTime
gst_cef_src_wait_tagged_frame (GstCefSrc *src, GstBuffer **out, gint64 *paint_tag, gint64 *squeeze_tag)
{
  GMutex *lock = GST_OBJECT_GET_LOCK (src);
  CefFrameRequest *req;
  GstClockTime running_time;
  gint64 want;
  gint64 deadline;

  *out = NULL;
  *paint_tag = -1;
  *squeeze_tag = -1;
  while (g_queue_is_empty (src->frame_requests) && !src->flushing)
    g_cond_wait (&src->frame_cond, lock);
  if (src->flushing)
    return GST_CLOCK_TIME_NONE;

  req = (CefFrameRequest *) g_queue_pop_head (src->frame_requests);
  running_time = req->running_time;
  want = req->tag & FRAME_TAG_MASK;
  g_free (req);

  /* Wait until a paint at or after the wanted tag exists. When the page keeps
   * painting but no tag reads (a graphic over it, a blend), stop waiting:
   * the newest paint goes out unpaired, instead of an older tagged paint for
   * ever (a frozen overlay). */
  deadline = g_get_monotonic_time () + (gint64) (src->frame_lock_timeout / GST_USECOND);
  while (!src->flushing) {
    if (src->tag_count > 0 && gst_cef_tag_not_before (src->tag_ids[src->tag_count - 1], want))
      break;
    if (gst_cef_src_tags_lost (src))
      break;
    if (!g_cond_wait_until (&src->frame_cond, lock, deadline))
      break;
  }
  if (src->flushing)
    return GST_CLOCK_TIME_NONE;

  /* Exact paint, else the newest paint before it (a repeat). Unpaired, *out
   * stays NULL and the newest paint goes out. */
  gint best = -1;
  gboolean unpaired = gst_cef_src_tags_lost (src);
  if (unpaired)
    src->ft_unpaired++;
  for (gint i = src->tag_count - 1; i >= 0 && !unpaired; i--) {
    if (src->tag_ids[i] == want) {
      best = i;
      src->ft_exact++;
      break;
    }
  }
  if (best < 0 && !unpaired) {
    for (gint i = src->tag_count - 1; i >= 0; i--) {
      if (!gst_cef_tag_not_before (src->tag_ids[i], want + 1)) {
        best = i;
        src->ft_stale++;
        break;
      }
    }
  }
  if (best < 0 && !unpaired && src->tag_count > 0) {
    best = 0;
    src->ft_ahead++;
  }
  if (best >= 0) {
    *out = gst_buffer_ref (src->tag_bufs[best]);
    *paint_tag = src->tag_ids[best];
    *squeeze_tag = src->tag_squeeze[best];
  }

  src->fl_frames++;
  if (src->fl_frames % 250 == 0) {
    GST_CAT_INFO_OBJECT (cef_lock_debug, src,
        "tagged frames %" G_GUINT64_FORMAT " exact %" G_GUINT64_FORMAT " stale %" G_GUINT64_FORMAT
        " ahead %" G_GUINT64_FORMAT " unpaired %" G_GUINT64_FORMAT " untagged paints %" G_GUINT64_FORMAT
        " dropped requests %" G_GUINT64_FORMAT " paired without paint-frame time %" G_GUINT64_FORMAT
        " with squeeze-frame time %" G_GUINT64_FORMAT,
        src->fl_frames, src->ft_exact, src->ft_stale, src->ft_ahead, src->ft_unpaired, src->ft_untagged,
        src->fl_dropped_requests, src->ft_unstamped, src->ft_squeeze);
  }
  return running_time;
}

static void
gst_cef_src_queue_request (GstCefSrc *src, guint64 running_time, gint64 tag)
{
  CefFrameRequest *req = g_new (CefFrameRequest, 1);
  req->running_time = running_time;
  req->tag = tag;

  GST_OBJECT_LOCK (src);
  if (tag >= 0)
    gst_cef_tag_times_put (&src->tag_times, (guint32) (tag & FRAME_TAG_MASK), running_time);
  if (g_queue_get_length (src->frame_requests) >= FRAME_LOCK_MAX_PENDING) {
    g_free (g_queue_pop_head (src->frame_requests));
    src->fl_dropped_requests++;
  }
  g_queue_push_tail (src->frame_requests, req);
  g_cond_broadcast (&src->frame_cond);
  GST_OBJECT_UNLOCK (src);
}

static void
gst_cef_src_request_frame (GstCefSrc *src, guint64 running_time)
{
  gst_cef_src_queue_request (src, running_time, -1);
}

static void
gst_cef_src_request_tagged_frame (GstCefSrc *src, guint64 running_time, guint tag)
{
  gst_cef_src_queue_request (src, running_time, tag);
}

static GstFlowReturn gst_cef_src_create(GstPushSrc *push_src, GstBuffer **buf)
{
  GstCefSrc *src = GST_CEF_SRC (push_src);
  GList *tmp;
  GstClockTime locked_rt = GST_CLOCK_TIME_NONE;
  GstBuffer *tagged = NULL;
  gint64 paint_tag = -1;
  gint64 squeeze_tag = -1;

  GST_OBJECT_LOCK (src);

  if (src->frame_tag) {
    locked_rt = gst_cef_src_wait_tagged_frame (src, &tagged, &paint_tag, &squeeze_tag);
    if (!GST_CLOCK_TIME_IS_VALID (locked_rt)) {
      GST_OBJECT_UNLOCK (src);
      return GST_FLOW_FLUSHING;
    }
  } else if (src->frame_lock) {
    locked_rt = gst_cef_src_wait_locked_frame (src);
    if (!GST_CLOCK_TIME_IS_VALID (locked_rt)) {
      GST_OBJECT_UNLOCK (src);
      return GST_FLOW_FLUSHING;
    }
  }

  if (src->audio_events) {
    for (tmp = src->audio_events; tmp; tmp = tmp->next) {
      gst_pad_push_event (GST_BASE_SRC_PAD (src), (GstEvent *) tmp->data);
    }

    g_list_free (src->audio_events);
    src->audio_events = NULL;
  }

  g_assert (src->current_buffer);
  if (tagged) {
    guint64 paint_rt;

    *buf = gst_buffer_copy (tagged);
    gst_buffer_unref (tagged);
    if (gst_cef_tag_times_get (&src->tag_times, (guint32) paint_tag, &paint_rt))
      gst_buffer_add_reference_timestamp_meta (*buf, src->paint_frame_caps, paint_rt, GST_CLOCK_TIME_NONE);
    else
      src->ft_unstamped++;
    if (squeeze_tag >= 0 && gst_cef_tag_times_get (&src->tag_times, (guint32) squeeze_tag, &paint_rt)) {
      gst_buffer_add_reference_timestamp_meta (*buf, src->squeeze_frame_caps, paint_rt, GST_CLOCK_TIME_NONE);
      src->ft_squeeze++;
    }
  } else {
    *buf = gst_buffer_copy (src->current_buffer);
  }

  if (src->audio_buffers) {
    gst_buffer_add_cef_audio_meta (*buf, src->audio_buffers);
    src->audio_buffers = NULL;
  }

  if (src->frame_lock || src->frame_tag) {
    GST_BUFFER_PTS (*buf) = locked_rt;
    GST_BUFFER_DTS (*buf) = locked_rt;
  } else {
    GST_BUFFER_PTS (*buf) = gst_util_uint64_scale (src->n_frames, src->vinfo.fps_d * GST_SECOND, src->vinfo.fps_n);
  }
  GST_BUFFER_DURATION (*buf) = gst_util_uint64_scale (GST_SECOND, src->vinfo.fps_d, src->vinfo.fps_n);
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
  src->flushing = FALSE;
  src->paint_seq = 0;
  src->fl_frames = src->fl_timeouts = src->fl_dropped_requests = 0;
  src->fl_paint_us_sum = src->fl_paint_us_max = src->fl_begin_frame_at = 0;
  src->ft_exact = src->ft_stale = src->ft_ahead = src->ft_untagged = src->ft_unpaired = src->ft_unstamped = src->ft_squeeze = 0;
  src->last_tagged_us = src->last_untagged_us = 0;
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

  gst_buffer_replace (&src->current_buffer, NULL);

  GST_OBJECT_LOCK (src);
  g_queue_clear_full (src->frame_requests, g_free);
  for (guint i = 0; i < src->tag_count; i++)
    gst_buffer_unref (src->tag_bufs[i]);
  src->tag_count = 0;
  memset (&src->tag_times, 0, sizeof (src->tag_times));
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static gboolean
gst_cef_src_unlock (GstBaseSrc * base_src)
{
  GstCefSrc *src = GST_CEF_SRC (base_src);

  GST_OBJECT_LOCK (src);
  src->flushing = TRUE;
  g_cond_broadcast (&src->frame_cond);
  GST_OBJECT_UNLOCK (src);
  return TRUE;
}

static gboolean
gst_cef_src_unlock_stop (GstBaseSrc * base_src)
{
  GstCefSrc *src = GST_CEF_SRC (base_src);

  GST_OBJECT_LOCK (src);
  src->flushing = FALSE;
  GST_OBJECT_UNLOCK (src);
  return TRUE;
}

static void
gst_cef_src_get_times (GstBaseSrc * base_src, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  GstClockTime timestamp = GST_BUFFER_PTS (buffer);
  GstClockTime duration = GST_BUFFER_DURATION (buffer);

  /* A locked frame is produced after its running time has passed; the
   * downstream aggregator waits for it within the reported latency. */
  if (GST_CEF_SRC (base_src)->frame_lock || GST_CEF_SRC (base_src)->frame_tag) {
    *start = *end = GST_CLOCK_TIME_NONE;
    return;
  }

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
    case GST_QUERY_LATENCY:
    {
      GstClockTime latency;

      if (src->frame_lock || src->frame_tag) {
        GST_DEBUG_OBJECT (src, "Reporting frame-lock latency: %" GST_TIME_FORMAT,
            GST_TIME_ARGS (src->frame_lock_latency));
        gst_query_set_latency (query, TRUE, src->frame_lock_latency, GST_CLOCK_TIME_NONE);
      } else if (src->vinfo.fps_n) {
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

  GST_OBJECT_LOCK (src);
  gst_video_info_from_caps (&src->vinfo, caps);
  new_buffer = gst_buffer_new_allocate (NULL, src->vinfo.width * src->vinfo.height * 4, NULL);
  gst_buffer_replace (&(src->current_buffer), new_buffer);
  gst_buffer_unref (new_buffer);
  if (src->browser) {
    CefRefPtr<CefBrowser> browser = src->browser;
    int fps = (int)gst_util_uint64_scale (1, src->vinfo.fps_n, src->vinfo.fps_d);
    /* In frame-lock mode the pipeline's requests set the paint rate. */
    if (!src->frame_lock && !src->frame_tag && src->paint_rate > 0 && src->paint_rate < fps) {
      GST_INFO_OBJECT (src, "Capping browser paint rate to %d (stream fps %d)",
          src->paint_rate, fps);
      fps = src->paint_rate;
    }
    /* In frame-tag mode the page draws one queued data frame per animation
     * frame. At exactly the stream rate a backlog (start-up, a slow frame)
     * never drains, so let the browser run faster; CEF clamps the rate. */
    if (src->frame_tag) {
      GST_INFO_OBJECT (src, "Frame tags: browser frame rate %d (stream fps %d)",
          fps * 2, fps);
      fps *= 2;
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
    case PROP_FRAME_LOCK:
      src->frame_lock = g_value_get_boolean (value);
      break;
    case PROP_FRAME_LOCK_LATENCY:
      src->frame_lock_latency = g_value_get_uint64 (value);
      break;
    case PROP_FRAME_LOCK_TIMEOUT:
      src->frame_lock_timeout = g_value_get_uint64 (value);
      break;
    case PROP_FRAME_TAG:
      src->frame_tag = g_value_get_boolean (value);
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
    case PROP_GPU:
      g_value_set_boolean (value, src->gpu);
      break;
    case PROP_CHROMIUM_DEBUG_PORT:
      g_value_set_int (value, src->chromium_debug_port);
      break;
    case PROP_PAINT_RATE:
      g_value_set_int (value, src->paint_rate);
      break;
    case PROP_FRAME_LOCK:
      g_value_set_boolean (value, src->frame_lock);
      break;
    case PROP_FRAME_LOCK_LATENCY:
      g_value_set_uint64 (value, src->frame_lock_latency);
      break;
    case PROP_FRAME_LOCK_TIMEOUT:
      g_value_set_uint64 (value, src->frame_lock_timeout);
      break;
    case PROP_FRAME_TAG:
      g_value_set_boolean (value, src->frame_tag);
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

  if (src->audio_buffers) {
    gst_buffer_list_unref (src->audio_buffers);
    src->audio_buffers = NULL;
  }

  g_list_free_full (src->audio_events, (GDestroyNotify) gst_event_unref);
  src->audio_events = NULL;

  g_free (src->js_flags);
  g_free (src->cef_cache_location);

  g_queue_free_full (src->frame_requests, g_free);
  gst_caps_unref (src->paint_frame_caps);
  gst_caps_unref (src->squeeze_frame_caps);
  g_cond_clear (&src->frame_cond);

  g_cond_clear(&src->state_cond);
  g_mutex_clear(&src->state_lock);
}

static void
gst_cef_src_init (GstCefSrc * src)
{
  GstBaseSrc *base_src = GST_BASE_SRC (src);

  src->n_frames = 0;
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
  src->frame_lock = DEFAULT_FRAME_LOCK;
  src->frame_lock_latency = DEFAULT_FRAME_LOCK_LATENCY;
  src->frame_lock_timeout = DEFAULT_FRAME_LOCK_TIMEOUT;
  src->frame_requests = g_queue_new ();
  src->frame_tag = FALSE;
  src->tag_count = 0;
  memset (&src->tag_times, 0, sizeof (src->tag_times));
  src->paint_frame_caps = gst_caps_new_empty_simple (GST_CEF_PAINT_FRAME_CAPS);
  src->squeeze_frame_caps = gst_caps_new_empty_simple (GST_CEF_SQUEEZE_FRAME_CAPS);
  g_cond_init (&src->frame_cond);
  src->paint_seq = 0;
  src->flushing = FALSE;

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

  g_object_class_install_property (gobject_class, PROP_FRAME_LOCK,
    g_param_spec_boolean ("frame-lock", "frame-lock",
          "Paint only on request: each request-frame call issues one external "
          "BeginFrame and the resulting buffer carries the requested running time. "
          "Process-global browser switches follow the first cefsrc.",
          DEFAULT_FRAME_LOCK,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_FRAME_LOCK_LATENCY,
    g_param_spec_uint64 ("frame-lock-latency", "frame-lock-latency",
          "Latency (ns) reported in frame-lock mode",
          0, G_MAXUINT64, DEFAULT_FRAME_LOCK_LATENCY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_FRAME_LOCK_TIMEOUT,
    g_param_spec_uint64 ("frame-lock-timeout", "frame-lock-timeout",
          "How long (ns) a locked frame waits for its paint before the last "
          "frame is repeated",
          0, G_MAXUINT64, DEFAULT_FRAME_LOCK_TIMEOUT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property (gobject_class, PROP_FRAME_TAG,
    g_param_spec_boolean ("frame-tag", "frame-tag",
          "The page stamps each paint with a checked frame tag (11 px, bottom-right, "
          "gstceftag.h); each request-tagged-frame call outputs the paint with that tag, "
          "stamped with the requested running time. A paired buffer whose paint's tag was "
          "requested within the last FRAME_TAG_TIMES (64) requests also carries a "
          "GstReferenceTimestampMeta (timestamp/x-ligr-paint-frame) with the running time "
          "of the frame its paint shows. A paint with a squeeze tag at the bottom left "
          "adds a second one (timestamp/x-ligr-squeeze-frame) for the frame that tag names. "
          "The browser paints freely. While paints come "
          "without a readable tag, the newest paint goes out unpaired, with no such meta.",
          FALSE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  /**
   * GstCefSrc::request-tagged-frame:
   * @src: the cefsrc
   * @running_time: running time (ns) of the video frame
   * @tag: 24-bit tag the page draws with that frame's data
   *
   * Frame-tag mode only.
   */
  gst_cef_src_signals[SIGNAL_REQUEST_TAGGED_FRAME] =
      g_signal_new_class_handler ("request-tagged-frame", G_TYPE_FROM_CLASS (klass),
      (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
      G_CALLBACK (gst_cef_src_request_tagged_frame), NULL, NULL, NULL,
      G_TYPE_NONE, 2, G_TYPE_UINT64, G_TYPE_UINT);

  /**
   * GstCefSrc::request-frame:
   * @src: the cefsrc
   * @running_time: running time (ns) of the video frame to render
   *
   * Frame-lock mode only. Queues one frame. The page must already hold the
   * data for this frame (for example, set over the DevTools protocol).
   */
  gst_cef_src_signals[SIGNAL_REQUEST_FRAME] =
      g_signal_new_class_handler ("request-frame", G_TYPE_FROM_CLASS (klass),
      (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
      G_CALLBACK (gst_cef_src_request_frame), NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_UINT64);

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
  base_src_class->unlock = GST_DEBUG_FUNCPTR(gst_cef_src_unlock);
  base_src_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_cef_src_unlock_stop);

  gstelement_class->change_state = GST_DEBUG_FUNCPTR(gst_cef_src_change_state);

  push_src_class->create = GST_DEBUG_FUNCPTR(gst_cef_src_create);

  GST_DEBUG_CATEGORY_INIT (cef_src_debug, "cefsrc", 0,
      "Chromium Embedded Framework Source");
  GST_DEBUG_CATEGORY_INIT (cef_console_debug, "cefconsole", 0,
      "Chromium Embedded Framework JS Console");
  GST_DEBUG_CATEGORY_INIT (cef_lock_debug, "ceflock", 0,
      "Chromium Embedded Framework frame lock");
}