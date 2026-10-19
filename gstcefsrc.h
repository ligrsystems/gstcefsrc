#ifndef __GST_CEF_SRC_H__
#define __GST_CEF_SRC_H__

/* Caps of the GstReferenceTimestampMeta on frame-tag output buffers: its
 * timestamp is the running time of the frame the buffer's paint shows. */
#define GST_CEF_PAINT_FRAME_CAPS "timestamp/x-ligr-paint-frame"

#include "include/cef_browser_process_handler.h"
#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>
#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_render_handler.h>
#include <include/cef_life_span_handler.h>
#include <include/cef_load_handler.h>
#include <include/wrapper/cef_helpers.h>
#include "gstceftag.h"

#ifdef _WIN32
class D3D11TextureReader;
#endif

G_BEGIN_DECLS

#define GST_TYPE_CEF_SRC \
  (gst_cef_src_get_type())
#define GST_CEF_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_CEF_SRC,GstCefSrc))
#define GST_CEF_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_CEF_SRC,GstCefSrcClass))
#define GST_IS_CEF_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_CEF_SRC))
#define GST_IS_CEF_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_CEF_SRC))

typedef struct _GstCefSrc GstCefSrc;
typedef struct _GstCefSrcClass GstCefSrcClass;

typedef enum : guint8 {
  // browser app not yet initialized
  CEF_SRC_CLOSED            = 0,
  // browser app initialized
  CEF_SRC_OPEN              = 1,
  // following states only possible if `listen_for_js_signals`:
  // waiting for CEF browser to send "ready" message (using window.gstSendMsg)
  CEF_SRC_WAITING_FOR_READY = 2,
  // received ready signal from webpage
  CEF_SRC_READY             = 3,
} CefSrcState;

#define CefSrcStateIsOpen(state) (state >= CEF_SRC_OPEN)

struct _GstCefSrc {
  GstPushSrc parent;
  GstBuffer *current_buffer;
  GstBufferList *audio_buffers;
  GList *audio_events;
  GstVideoInfo vinfo;
  guint64 n_frames;
  gulong cef_work_id;
  gchar *url;
  gchar *chrome_extra_flags;
  gchar *js_flags;
  cef_log_severity_t log_severity;
  gchar *cef_cache_location;
  gboolean gpu;
  gboolean sandbox;
  gboolean listen_for_js_signals;
  gint chromium_debug_port;
  gint paint_rate;
  /* Frame lock: the browser paints only when the pipeline asks for a frame.
   * Each request-frame call carries the running time the frame belongs to;
   * create() sends one external BeginFrame for it, waits for the paint and
   * stamps the buffer with that running time. Guarded by the object lock. */
  gboolean frame_lock;
  GstClockTime frame_lock_latency;
  GstClockTime frame_lock_timeout;
  GQueue *frame_requests;
  GCond frame_cond;
  guint64 paint_seq;
  gboolean flushing;
  guint64 fl_frames;
  guint64 fl_timeouts;
  guint64 fl_dropped_requests;
  gint64 fl_paint_us_sum;
  gint64 fl_paint_us_max;
  gint64 fl_begin_frame_at;
  guint64 fl_paints_window;
  gint64 fl_paints_window_start;

  /* Frame tag: the browser paints freely; the page stamps each paint with the
   * tag of the data frame it shows (9 px, bottom-right). OnPaint reads and
   * clears the tag and keeps the latest paints; create() outputs the paint
   * whose tag matches each request. */
  gboolean frame_tag;
  GstBuffer *tag_bufs[16];
  gint64 tag_ids[16];
  guint tag_count;
  guint64 ft_exact;
  guint64 ft_stale;
  guint64 ft_ahead;
  guint64 ft_untagged;
  /* Frames sent unpaired: paints kept coming without a readable tag. */
  guint64 ft_unpaired;
  /* The running time each recent tag was requested for. Every paired output
   * buffer carries a GstReferenceTimestampMeta (caps GST_CEF_PAINT_FRAME_CAPS)
   * with the running time of the frame its paint shows, so a downstream
   * compositor knows which frame's data the overlay drew (a repeat or an
   * ahead paint differs from the buffer's PTS). Unpaired buffers carry none. */
  GstCefTagTimes tag_times;
  GstCaps *paint_frame_caps;
  /* Monotonic time of the last paint with and without a readable tag. */
  gint64 last_tagged_us;
  gint64 last_untagged_us;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefApp> app;

  GCond state_cond;
  GMutex state_lock;
  CefSrcState state;

#ifdef _WIN32
  D3D11TextureReader* texture_reader;
#endif
};

struct _GstCefSrcClass {
  GstPushSrcClass parent_class;
};

class BrowserApp : public CefApp, public CefBrowserProcessHandler {
public:
  BrowserApp(GstCefSrc *src);

  void OnBeforeCommandLineProcessing(const CefString &process_type,
                                     CefRefPtr<CefCommandLine> command_line) override;

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
  void OnContextInitialized() override;
#ifdef __APPLE__
  void OnScheduleMessagePumpWork(int64_t delay_ms) override;
#endif
private:
  IMPLEMENT_REFCOUNTING(BrowserApp);
  GstCefSrc *src;
};

GType gst_cef_src_get_type (void);

G_END_DECLS

#endif /* __GST_CEF_SRC_H__ */

