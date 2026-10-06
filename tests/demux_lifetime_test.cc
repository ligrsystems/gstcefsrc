#include "gstcefdemux.h"
#include <cassert>
#include <iostream>

static GstBuffer* downstream = nullptr;
static GstClockTime gap_time = GST_CLOCK_TIME_NONE, gap_duration = GST_CLOCK_TIME_NONE;
static GstFlowReturn take_video(GstPad*, GstObject*, GstBuffer* buffer) {
  downstream = buffer;
  // Ownership has transferred. Downstream can now change its writable metadata.
  GST_BUFFER_PTS(buffer) = 99 * GST_SECOND;
  GST_BUFFER_DURATION(buffer) = 7 * GST_SECOND;
  return GST_FLOW_OK;
}
static gboolean take_event(GstPad*, GstObject*, GstEvent* event) {
  if (GST_EVENT_TYPE(event) == GST_EVENT_GAP)
    gst_event_parse_gap(event, &gap_time, &gap_duration);
  gst_event_unref(event);
  return TRUE;
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstElement* demux = GST_ELEMENT(g_object_new(GST_TYPE_CEF_DEMUX, nullptr));
  GstPad* video = gst_element_get_static_pad(demux, "video");
  GstPad* audio = gst_element_get_static_pad(demux, "audio");
  GstPad* sink = gst_element_get_static_pad(demux, "sink");
  GstPad* video_sink = gst_pad_new("video-sink", GST_PAD_SINK);
  GstPad* audio_sink = gst_pad_new("audio-sink", GST_PAD_SINK);
  gst_pad_set_chain_function(video_sink, take_video);
  gst_pad_set_event_function(video_sink, take_event);
  gst_pad_set_event_function(audio_sink, take_event);
  gst_pad_set_active(video_sink, TRUE); gst_pad_set_active(audio_sink, TRUE);
  assert(gst_pad_link(video, video_sink) == GST_PAD_LINK_OK);
  assert(gst_pad_link(audio, audio_sink) == GST_PAD_LINK_OK);
  assert(gst_element_set_state(demux, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  gst_pad_send_event(sink, gst_event_new_stream_start("test"));
  GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA,width=2,height=2,framerate=30/1");
  gst_pad_send_event(sink, gst_event_new_caps(caps)); gst_caps_unref(caps);
  GstSegment segment; gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_send_event(sink, gst_event_new_segment(&segment));
  GstBuffer* frame = gst_buffer_new_allocate(nullptr, 16, nullptr);
  GST_BUFFER_PTS(frame) = 5 * GST_SECOND;
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  assert(gst_pad_chain(sink, frame) == GST_FLOW_OK);
  assert(gap_time == 5 * GST_SECOND);
  assert(gap_duration == GST_SECOND / 30);
  gst_buffer_unref(downstream);
  gst_element_set_state(demux, GST_STATE_NULL);
  gst_pad_unlink(video, video_sink); gst_pad_unlink(audio, audio_sink);
  gst_pad_set_active(video_sink, FALSE); gst_pad_set_active(audio_sink, FALSE);
  gst_object_unref(video); gst_object_unref(audio); gst_object_unref(sink);
  gst_object_unref(video_sink); gst_object_unref(audio_sink); gst_object_unref(demux);
  std::cout << "Demux transferred-buffer lifetime test passed\n";
}
