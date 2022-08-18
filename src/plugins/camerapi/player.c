#include "gst/gstelement.h"
#include "gst/gstmessage.h"
#define _GNU_SOURCE

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/video/gstvideometa.h>

#include <collection.h>
#include <flutter-pi.h>
#include <notifier_listener.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <plugins/camerapi.h>
#include <texture_registry.h>

FILE_DESCR("gstreamer video_player")

#ifdef DEBUG
#define DEBUG_TRACE_BEGIN(player, name) trace_begin(player, name)
#define DEBUG_TRACE_END(player, name) trace_end(player, name)
#define DEBUG_TRACE_INSTANT(player, name) trace_instant(player, name)
#else
#define DEBUG_TRACE_BEGIN(player, name) \
    do {                                \
    } while (0)
#define DEBUG_TRACE_END(player, name) \
    do {                              \
    } while (0)
#define DEBUG_TRACE_INSTANT(player, name) \
    do {                                  \
    } while (0)
#endif

#define LOG_GST_SET_STATE_ERROR(_element)                                                                             \
    LOG_ERROR(                                                                                                        \
      "setting gstreamer playback state failed. gst_element_set_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
      GST_ELEMENT_NAME(_element))
#define LOG_GST_GET_STATE_ERROR(_element)                                                                        \
    LOG_ERROR(                                                                                                   \
      "last gstreamer state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n", \
      GST_ELEMENT_NAME(_element))

struct incomplete_video_info
{
    bool has_resolution;
    bool has_fps;
    struct camera_video_info info;
};

struct camerapi
{
    pthread_mutex_t lock;

    struct flutterpi *flutterpi;
    void *userdata;

    bool is_forcing_sw_decoding;
    bool is_currently_falling_back_to_sw_decoding;

    struct notifier video_info_notifier, buffering_state_notifier, error_notifier;

    bool has_sent_info;
    struct incomplete_video_info info;

    bool has_gst_info;
    GstVideoInfo gst_info;

    struct texture *texture;
    int64_t texture_id;

    struct frame_interface *frame_interface;

    GstElement *pipeline, *sink;
    GstBus *bus;
    sd_event_source *busfd_events;
    uint32_t drm_format;
    bool has_drm_modifier;
    uint64_t drm_modifier;
    EGLint egl_color_space;
};

#define MAX_N_PLANES 4
#define MAX_N_EGL_DMABUF_IMAGE_ATTRIBUTES 6 + 6 * MAX_N_PLANES + 1

static inline void lock(struct camerapi *player)
{
    pthread_mutex_lock(&player->lock);
}

static inline void unlock(struct camerapi *player)
{
    pthread_mutex_unlock(&player->lock);
}

static inline void trace_instant(struct camerapi *player, const char *name)
{
    return flutterpi_trace_event_instant(player->flutterpi, name);
}

static inline void trace_begin(struct camerapi *player, const char *name)
{
    return flutterpi_trace_event_begin(player->flutterpi, name);
}

static inline void trace_end(struct camerapi *player, const char *name)
{
    return flutterpi_trace_event_end(player->flutterpi, name);
}

static int maybe_send_info(struct camerapi *player)
{
    struct camera_video_info *duped;

    if (player->info.has_resolution && player->info.has_fps) {
        // we didn't send the info yet but we have complete video info now.
        // send it!
        duped = memdup(&(player->info.info), sizeof(player->info.info));
        if (duped == NULL) {
            return ENOMEM;
        }

        notifier_notify(&player->video_info_notifier, duped);
    }
    return 0;
}

static void update_buffering_state(struct camerapi *player)
{
    struct buffering_state *state;
    GstBufferingMode mode;
    GstQuery *query;
    gboolean ok, busy;
    int64_t start, stop, buffering_left;
    int n_ranges, percent, avg_in, avg_out;

    query = gst_query_new_buffering(GST_FORMAT_TIME);
    ok = gst_element_query(player->pipeline, query);
    if (ok == FALSE) {
        LOG_ERROR("Could not query buffering state. (gst_element_query)\n");
        return;
    }

    gst_query_parse_buffering_percent(query, &busy, &percent);
    gst_query_parse_buffering_stats(query, &mode, &avg_in, &avg_out, &buffering_left);

    n_ranges = (int)gst_query_get_n_buffering_ranges(query);

    state = malloc(sizeof(*state) + n_ranges * sizeof(struct buffering_range));
    if (state == NULL) {
        return;
    }

    for (int i = 0; i < n_ranges; i++) {
        ok = gst_query_parse_nth_buffering_range(query, (unsigned int)i, &start, &stop);
        if (ok == FALSE) {
            LOG_ERROR("Could not parse %dth buffering range from buffering state. (gst_query_parse_nth_buffering_range)\n", i);
            return;
        }

        state->ranges[i].start_ms = GST_TIME_AS_MSECONDS(start);
        state->ranges[i].stop_ms = GST_TIME_AS_MSECONDS(stop);
    }

    gst_query_unref(query);

    state->percent = percent;
    state->mode =
      (mode == GST_BUFFERING_STREAM      ? kStream
       : mode == GST_BUFFERING_DOWNLOAD  ? kDownload
       : mode == GST_BUFFERING_TIMESHIFT ? kTimeshift
       : mode == GST_BUFFERING_LIVE      ? kLive
                                         : (assert(0), kStream));
    state->avg_in = avg_in;
    state->avg_out = avg_out;
    state->time_left_ms = buffering_left;
    state->n_ranges = n_ranges;

    notifier_notify(&player->buffering_state_notifier, state);
}

static int init_camera(struct camerapi *player, bool force_sw_decoders);

static void maybe_deinit(struct camerapi *player);

static void fallback_to_sw_decoding(struct camerapi *player)
{
    maybe_deinit(player);
    player->is_currently_falling_back_to_sw_decoding = true;
    init_camera(player, true);
}

static int apply_playback_state(struct camerapi *player)
{
    GstStateChangeReturn ok;
    GstState current_state, pending_state;

    // if we're currently falling back to software decoding, don't do anything.
    if (player->is_currently_falling_back_to_sw_decoding) {
        return 0;
    }

    DEBUG_TRACE_BEGIN(player, "gst_element_get_state");
    ok = gst_element_get_state(player->pipeline, &current_state, &pending_state, 0);
    DEBUG_TRACE_END(player, "gst_element_get_state");

    if (ok == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR(
          "last gstreamer pipeline state change failed. gst_element_get_state(element name: %s): GST_STATE_CHANGE_FAILURE\n",
          GST_ELEMENT_NAME(player->pipeline));
        DEBUG_TRACE_END(player, "apply_playback_state");
        return EIO;
    }

    if (pending_state == GST_STATE_VOID_PENDING) {
        if (current_state == GST_STATE_PLAYING) {
            // we're already in the desired state, and we're also not changing it
            // no need to do anything.
            LOG_DEBUG("apply_playback_state: already in playing state and none pending\n");
            DEBUG_TRACE_END(player, "apply_playback_state");
            return 0;
        }

        LOG_DEBUG("apply_playback_state: setting state to playing\n");

        DEBUG_TRACE_BEGIN(player, "gst_element_set_state");
        ok = gst_element_set_state(player->pipeline, GST_STATE_PLAYING);
        DEBUG_TRACE_END(player, "gst_element_set_state");

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player->pipeline);
            DEBUG_TRACE_END(player, "apply_playback_state");
            return EIO;
        }
    } else if (pending_state != GST_STATE_PLAYING) {
        // queue to be executed when pending async state change completes
        /// TODO: Implement properly

        LOG_DEBUG("apply_playback_state: async state change in progress, setting state to playing\n");

        DEBUG_TRACE_BEGIN(player, "gst_element_set_state");
        ok = gst_element_set_state(player->pipeline, GST_STATE_PLAYING);
        DEBUG_TRACE_END(player, "gst_element_set_state");

        if (ok == GST_STATE_CHANGE_FAILURE) {
            LOG_GST_SET_STATE_ERROR(player->pipeline);
            DEBUG_TRACE_END(player, "apply_playback_state");
            return EIO;
        }
    }

    DEBUG_TRACE_END(player, "apply_playback_state");
    return 0;
}

static void on_bus_message(struct camerapi *player, GstMessage *msg)
{
    GstState old, current, pending, requested;
    GError *error;
    gchar *debug_info;

    LOG_DEBUG("on_bus_message %s\n", GST_MESSAGE_TYPE_NAME(msg));
    DEBUG_TRACE_BEGIN(player, "on_bus_message");
    if (gst_message_has_name(msg, "barcode")) {
        LOG_DEBUG("barcode detected\n");
    }
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &error, &debug_info);

            fprintf(
              stderr,
              "[gstreamer video player] gstreamer error: code: %d, domain: %s, msg: %s (debug info: %s)\n",
              error->code,
              g_quark_to_string(error->domain),
              error->message,
              debug_info);
            if (
              error->domain == GST_STREAM_ERROR && error->code == GST_STREAM_ERROR_DECODE &&
              strcmp(error->message, "No valid frames decoded before end of stream") == 0) {
                LOG_ERROR("Hardware decoder failed. Falling back to software decoding...\n");
                fallback_to_sw_decoding(player);
            }

            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(msg, &error, &debug_info);
            LOG_ERROR("gstreamer warning: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_INFO:
            gst_message_parse_info(msg, &error, &debug_info);
            LOG_DEBUG("gstreamer info: %s (debug info: %s)\n", error->message, debug_info);
            g_clear_error(&error);
            g_free(debug_info);
            break;

        case GST_MESSAGE_BUFFERING: {
            GstBufferingMode mode;
            int64_t buffering_left;
            int percent, avg_in, avg_out;

            gst_message_parse_buffering(msg, &percent);
            gst_message_parse_buffering_stats(msg, &mode, &avg_in, &avg_out, &buffering_left);

            LOG_DEBUG(
              "buffering, src: %s, percent: %d, mode: %s, avg in: %d B/s, avg out: %d B/s, %" GST_TIME_FORMAT "\n",
              GST_MESSAGE_SRC_NAME(msg),
              percent,
              mode == GST_BUFFERING_STREAM      ? "stream"
              : mode == GST_BUFFERING_DOWNLOAD  ? "download"
              : mode == GST_BUFFERING_TIMESHIFT ? "timeshift"
              : mode == GST_BUFFERING_LIVE      ? "live"
                                                : "?",
              avg_in,
              avg_out,
              GST_TIME_ARGS(buffering_left * GST_MSECOND));

            /// TODO: GST_MESSAGE_BUFFERING is only emitted when we actually need to wait on some buffering till we can resume the
            /// playback. However, the info we send to the callback also contains information on the buffered video ranges. That
            /// information is constantly changing, but we only notify the player about it when we actively wait for the buffer to
            /// be filled.
            DEBUG_TRACE_BEGIN(player, "update_buffering_state");
            update_buffering_state(player);
            DEBUG_TRACE_END(player, "update_buffering_state");

            break;
        };

        case GST_MESSAGE_STATE_CHANGED:
            gst_message_parse_state_changed(msg, &old, &current, &pending);
            LOG_DEBUG(
              "state-changed: src: %s, old: %s, current: %s, pending: %s\n",
              GST_MESSAGE_SRC_NAME(msg),
              gst_element_state_get_name(old),
              gst_element_state_get_name(current),
              gst_element_state_get_name(pending));

            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->pipeline)) {
                if (current == GST_STATE_PAUSED || current == GST_STATE_PLAYING) {
                    // it's our pipeline that changed to either playing / paused, and we don't have info about our video duration
                    // yet. get that info now. technically we can already fetch the duration when the decodebin changed to PAUSED
                    // state.
                    DEBUG_TRACE_BEGIN(player, "fetch video info");
                    maybe_send_info(player);
                    DEBUG_TRACE_END(player, "fetch video info");
                }
            }
            break;

        case GST_MESSAGE_ASYNC_DONE:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(player->pipeline) && player->is_currently_falling_back_to_sw_decoding) {
                player->is_currently_falling_back_to_sw_decoding = false;
                apply_playback_state(player);
            }
            break;

        case GST_MESSAGE_LATENCY:
            LOG_DEBUG("gstreamer: redistributing latency\n");
            DEBUG_TRACE_BEGIN(player, "gst_bin_recalculate_latency");
            gst_bin_recalculate_latency(GST_BIN(player->pipeline));
            DEBUG_TRACE_END(player, "gst_bin_recalculate_latency");
            break;

        case GST_MESSAGE_EOS:
            LOG_DEBUG("end of stream, src: %s\n", GST_MESSAGE_SRC_NAME(msg));
            break;

        case GST_MESSAGE_REQUEST_STATE:
            gst_message_parse_request_state(msg, &requested);
            LOG_DEBUG(
              "gstreamer state change to %s was requested by %s\n",
              gst_element_state_get_name(requested),
              GST_MESSAGE_SRC_NAME(msg));
            DEBUG_TRACE_BEGIN(player, "gst_element_set_state");
            gst_element_set_state(GST_ELEMENT(player->pipeline), requested);
            DEBUG_TRACE_END(player, "gst_element_set_state");
            break;

        case GST_MESSAGE_APPLICATION:
            LOG_DEBUG("Application message\n");
            break;

        default:
            LOG_DEBUG("gstreamer message: %s, src: %s\n", GST_MESSAGE_TYPE_NAME(msg), GST_MESSAGE_SRC_NAME(msg));
            break;
    }
    DEBUG_TRACE_END(player, "on_bus_message");
    return;
}

static int on_bus_fd_ready(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    struct camerapi *player;
    GstMessage *msg;

    (void)s;
    (void)fd;
    (void)revents;

    player = userdata;

    DEBUG_TRACE_BEGIN(player, "on_bus_fd_ready");

    msg = gst_bus_pop(player->bus);
    if (msg != NULL) {
        on_bus_message(player, msg);
        gst_message_unref(msg);
    }

    DEBUG_TRACE_END(player, "on_bus_fd_ready");

    return 0;
}

static GstPadProbeReturn on_query_appsink(GstPad *pad, GstPadProbeInfo *info, void *userdata)
{
    GstQuery *query;

    (void)pad;
    (void)userdata;

    query = info->data;

    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) {
        return GST_PAD_PROBE_OK;
    }

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);

    return GST_PAD_PROBE_HANDLED;
}

static GstPadProbeReturn on_probe_pad(GstPad *pad, GstPadProbeInfo *info, void *userdata)
{
    struct camerapi *player;
    GstVideoInfo vinfo;
    GstEvent *event;
    GstCaps *caps;
    gboolean ok;

    (void)pad;

    player = userdata;
    event = GST_PAD_PROBE_INFO_EVENT(info);

    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
        return GST_PAD_PROBE_OK;
    }

    gst_event_parse_caps(event, &caps);
    if (caps == NULL) {
        LOG_ERROR("gstreamer: caps event without caps\n");
        return GST_PAD_PROBE_OK;
    }

    ok = gst_video_info_from_caps(&vinfo, caps);
    if (!ok) {
        LOG_ERROR("gstreamer: caps event with invalid video caps\n");
        return GST_PAD_PROBE_OK;
    }

    switch (GST_VIDEO_INFO_FORMAT(&vinfo)) {
        case GST_VIDEO_FORMAT_Y42B:
            player->drm_format = DRM_FORMAT_YUV422;
            break;
        case GST_VIDEO_FORMAT_YV12:
            player->drm_format = DRM_FORMAT_YVU420;
            break;
        case GST_VIDEO_FORMAT_I420:
            player->drm_format = DRM_FORMAT_YUV420;
            break;
        case GST_VIDEO_FORMAT_NV12:
            player->drm_format = DRM_FORMAT_NV12;
            break;
        case GST_VIDEO_FORMAT_NV21:
            player->drm_format = DRM_FORMAT_NV21;
            break;
        case GST_VIDEO_FORMAT_YUY2:
            player->drm_format = DRM_FORMAT_YUYV;
            break;
        default:
            LOG_ERROR("unsupported video format: %s\n", GST_VIDEO_INFO_NAME(&vinfo));
            player->drm_format = 0;
            break;
    }

    const GstVideoColorimetry *color = &GST_VIDEO_INFO_COLORIMETRY(&vinfo);

    if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT601)) {
        player->egl_color_space = EGL_ITU_REC601_EXT;
    } else if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT709)) {
        player->egl_color_space = EGL_ITU_REC709_EXT;
    } else if (gst_video_colorimetry_matches(color, GST_VIDEO_COLORIMETRY_BT2020)) {
        player->egl_color_space = EGL_ITU_REC2020_EXT;
    } else {
        LOG_ERROR("unsupported video colorimetry: %s\n", gst_video_colorimetry_to_string(color));
        player->egl_color_space = EGL_NONE;
    }

    memcpy(&player->gst_info, &vinfo, sizeof vinfo);
    player->has_gst_info = true;

    LOG_DEBUG("on_probe_pad, fps: %f, res: % 4d x % 4d\n", (double)vinfo.fps_n / vinfo.fps_d, vinfo.width, vinfo.height);

    player->info.info.width = vinfo.width;
    player->info.info.height = vinfo.height;
    player->info.info.fps = (double)vinfo.fps_n / vinfo.fps_d;
    player->info.has_resolution = true;
    player->info.has_fps = true;
    maybe_send_info(player);

    return GST_PAD_PROBE_OK;
}

static void on_destroy_texture_frame(const struct texture_frame *texture_frame, void *userdata)
{
    struct video_frame *frame;

    (void)texture_frame;

    DEBUG_ASSERT_NOT_NULL(texture_frame);
    DEBUG_ASSERT_NOT_NULL(userdata);

    frame = userdata;

    frame_destroy(frame);
}

static void on_appsink_eos(GstAppSink *appsink, void *userdata)
{
    gboolean ok;

    DEBUG_ASSERT_NOT_NULL(appsink);
    DEBUG_ASSERT_NOT_NULL(userdata);

    (void)userdata;

    LOG_DEBUG("on_appsink_eos()\n");

    // this method is called from the streaming thread.
    // we shouldn't access the player directly here, it could change while we use it.
    // post a message to the gstreamer bus instead, will be handled by
    // @ref on_bus_message.
    ok = gst_element_post_message(
      GST_ELEMENT(appsink), gst_message_new_application(GST_OBJECT(appsink), gst_structure_new_empty("appsink-eos")));
    if (ok == FALSE) {
        LOG_ERROR("Could not post appsink end-of-stream event to the message bus.\n");
    }
}

static GstFlowReturn on_appsink_new_preroll(GstAppSink *appsink, void *userdata)
{
    struct video_frame *frame;
    struct camerapi *player;
    GstSample *sample;

    DEBUG_ASSERT_NOT_NULL(appsink);
    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    sample = gst_app_sink_try_pull_preroll(appsink, 0);
    if (sample == NULL) {
        LOG_ERROR("gstreamer returned a NULL sample.\n");
        return GST_FLOW_ERROR;
    }

    frame = frame_new(
      player->frame_interface,
      &(struct frame_info){
        .drm_format = player->drm_format, .egl_color_space = player->egl_color_space, .gst_info = &player->gst_info },
      sample);

    if (frame != NULL) {
        texture_push_frame(
          player->texture,
          &(struct texture_frame){
            .gl = *frame_get_gl_frame(frame),
            .destroy = on_destroy_texture_frame,
            .userdata = frame,
          });
    }

    return GST_FLOW_OK;
}

static GstFlowReturn on_appsink_new_sample(GstAppSink *appsink, void *userdata)
{
    struct video_frame *frame;
    struct camerapi *player;
    GstSample *sample;

    DEBUG_ASSERT_NOT_NULL(appsink);
    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    sample = gst_app_sink_try_pull_sample(appsink, 0);
    if (sample == NULL) {
        LOG_ERROR("gstreamer returned a NULL sample.\n");
        return GST_FLOW_ERROR;
    }

    frame = frame_new(
      player->frame_interface,
      &(struct frame_info){
        .drm_format = player->drm_format, .egl_color_space = player->egl_color_space, .gst_info = &player->gst_info },
      sample);

    if (frame != NULL) {
        texture_push_frame(
          player->texture,
          &(struct texture_frame){
            .gl = *frame_get_gl_frame(frame),
            .destroy = on_destroy_texture_frame,
            .userdata = frame,
          });
    }

    return GST_FLOW_OK;
}

static void on_appsink_cbs_destroy(void *userdata)
{
    struct camerapi *player;

    LOG_DEBUG("on_appsink_cbs_destroy()\n");
    DEBUG_ASSERT_NOT_NULL(userdata);

    player = userdata;

    (void)player;
}

static int init_camera(struct camerapi *player, bool force_sw_decoders)
{
    sd_event_source *busfd_event_source;
    GstElement *pipeline, *sink;
    GstBus *bus;
    GstPad *pad;
    GPollFD fd;
    GError *error = NULL;
    int ok;

    static const char *pipeline_descr =
      "libcamerasrc ! queue ! videoconvert ! zbar name=zbar ! video/x-raw,framerate=0/1 ! videoconvert ! "
      "video/x-raw,format=I420 ! appsink sync=true name=\"camerasink\"";

    pipeline = gst_parse_launch(pipeline_descr, &error);
    if (pipeline == NULL) {
        LOG_ERROR("Could create GStreamer pipeline from description: %s (pipeline: `%s`)\n", error->message, pipeline_descr);
        return error->code;
    }

    sink = gst_bin_get_by_name(GST_BIN(pipeline), "camerasink");
    if (sink == NULL) {
        LOG_ERROR("Couldn't find appsink in pipeline bin.\n");
        ok = EINVAL;
        goto fail_unref_pipeline;
    }

    pad = gst_element_get_static_pad(sink, "sink");
    if (pad == NULL) {
        LOG_ERROR("Couldn't get static pad \"camerasink\" from video sink.\n");
        ok = EINVAL;
        goto fail_unref_sink;
    }

    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, on_query_appsink, player, NULL);

    gst_base_sink_set_max_lateness(GST_BASE_SINK(sink), 20 * GST_MSECOND);
    gst_base_sink_set_qos_enabled(GST_BASE_SINK(sink), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 2);
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), TRUE);
    gst_app_sink_set_drop(GST_APP_SINK(sink), FALSE);

    gst_app_sink_set_callbacks(
      GST_APP_SINK(sink),
      &(GstAppSinkCallbacks){ .eos = on_appsink_eos,
                              .new_preroll = on_appsink_new_preroll,
                              .new_sample = on_appsink_new_sample,
                              ._gst_reserved = { 0 } },
      player,
      on_appsink_cbs_destroy);

    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_probe_pad, player, NULL);

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));

    gst_bus_get_pollfd(bus, &fd);

    flutterpi_sd_event_add_io(&busfd_event_source, fd.fd, EPOLLIN, on_bus_fd_ready, player);

    LOG_DEBUG("Setting state to paused...\n");
    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PAUSED);

    player->sink = sink;
    /// FIXME: Not sure we need this here. pipeline is floating after gst_parse_launch, which
    /// means we should take a reference, but the examples don't increase the refcount.
    player->pipeline = pipeline; // gst_object_ref(pipeline);
    player->bus = bus;
    player->busfd_events = busfd_event_source;
    player->is_forcing_sw_decoding = force_sw_decoders;

    gst_object_unref(pad);
    return 0;

fail_unref_sink:
    gst_object_unref(sink);

fail_unref_pipeline:
    gst_object_unref(pipeline);

    return ok;
}

static void maybe_deinit(struct camerapi *player)
{
    struct my_gst_object
    {
        GInitiallyUnowned object;

        /*< public >*/     /* with LOCK */
        GMutex lock;       /* object LOCK */
        gchar *name;       /* object name */
        GstObject *parent; /* this object's parent, weak ref */
        guint32 flags;

        /*< private >*/
        GList *control_bindings; /* List of GstControlBinding */
        guint64 control_rate;
        guint64 last_sync;

        gpointer _gst_reserved;
    };

    struct my_gst_object *sink = (struct my_gst_object *)player->sink, *bus = (struct my_gst_object *)player->bus,
                         *pipeline = (struct my_gst_object *)player->pipeline;
    (void)sink;
    (void)bus;
    (void)pipeline;

    if (player->busfd_events != NULL) {
        sd_event_source_unrefp(&player->busfd_events);
    }
    if (player->sink != NULL) {
        gst_object_unref(GST_OBJECT(player->sink));
        player->sink = NULL;
    }
    if (player->bus != NULL) {
        gst_object_unref(GST_OBJECT(player->bus));
        player->bus = NULL;
    }
    if (player->pipeline != NULL) {
        gst_element_set_state(GST_ELEMENT(player->pipeline), GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(player->pipeline));
        player->pipeline = NULL;
    }
}

DEFINE_LOCK_OPS(camerapi, lock)

struct camerapi *camerapi_new(struct flutterpi *flutterpi, void *userdata)
{
    struct frame_interface *frame_interface;
    struct camerapi *player;
    struct texture *texture;
    int64_t texture_id;
    int ok;

    player = malloc(sizeof *player);
    if (player == NULL)
        return NULL;

    texture = flutterpi_create_texture(flutterpi);
    if (texture == NULL)
        goto fail_free_player;

    frame_interface = frame_interface_new(flutterpi);
    if (frame_interface == NULL)
        goto fail_destroy_texture;

    texture_id = texture_get_id(texture);

    ok = pthread_mutex_init(&player->lock, NULL);
    if (ok != 0)
        goto fail_destroy_frame_interface;

    ok = value_notifier_init(&player->video_info_notifier, NULL, free /* free(NULL) is a no-op, I checked */);
    if (ok != 0)
        goto fail_destroy_mutex;

    ok = value_notifier_init(&player->buffering_state_notifier, NULL, free);
    if (ok != 0)
        goto fail_deinit_video_info_notifier;

    ok = change_notifier_init(&player->error_notifier);
    if (ok != 0)
        goto fail_deinit_buffering_state_notifier;

    player->flutterpi = flutterpi;
    player->userdata = userdata;
    player->is_forcing_sw_decoding = false;
    player->is_currently_falling_back_to_sw_decoding = false;
    player->has_sent_info = false;
    player->info.has_resolution = false;
    player->info.has_fps = false;
    player->has_gst_info = false;
    memset(&player->gst_info, 0, sizeof(player->gst_info));
    player->texture = texture;
    player->texture_id = texture_id;
    player->frame_interface = frame_interface;
    player->pipeline = NULL;
    player->sink = NULL;
    player->bus = NULL;
    player->busfd_events = NULL;
    player->drm_format = 0;
    return player;

    // fail_deinit_error_notifier:
    // notifier_deinit(&player->error_notifier);

fail_deinit_buffering_state_notifier:
    notifier_deinit(&player->buffering_state_notifier);

fail_deinit_video_info_notifier:
    notifier_deinit(&player->video_info_notifier);

fail_destroy_mutex:
    pthread_mutex_destroy(&player->lock);

fail_destroy_frame_interface:
    frame_interface_unref(frame_interface);

fail_destroy_texture:
    texture_destroy(texture);

fail_free_player:
    free(player);

    return NULL;
}

void camerapi_destroy(struct camerapi *player)
{
    LOG_DEBUG("camerapi_destroy(%p)\n", player);
    notifier_deinit(&player->video_info_notifier);
    notifier_deinit(&player->buffering_state_notifier);
    notifier_deinit(&player->error_notifier);
    maybe_deinit(player);
    pthread_mutex_destroy(&player->lock);
    frame_interface_unref(player->frame_interface);
    texture_destroy(player->texture);
    free(player);
}

int64_t camerapi_get_texture_id(struct camerapi *player)
{
    return player->texture_id;
}

void camerapi_set_userdata_locked(struct camerapi *player, void *userdata)
{
    player->userdata = userdata;
}

void *camerapi_get_userdata_locked(struct camerapi *player)
{
    return player->userdata;
}

int camerapi_initialize(struct camerapi *player)
{
    int res = init_camera(player, false);
    if (res != 0) {
        return res;
    }
    return apply_playback_state(player);
}

struct notifier *camerapi_get_video_info_notifier(struct camerapi *player)
{
    return &player->video_info_notifier;
}

struct notifier *camerapi_get_buffering_state_notifier(struct camerapi *player)
{
    return &player->buffering_state_notifier;
}

struct notifier *camerapi_get_error_notifier(struct camerapi *player)
{
    return &player->error_notifier;
}
