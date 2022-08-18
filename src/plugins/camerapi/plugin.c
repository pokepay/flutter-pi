#define _GNU_SOURCE

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include <gst/gst.h>
#include <gst/video/video-info.h>

#include <collection.h>
#include <flutter-pi.h>
#include <notifier_listener.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <plugins/camerapi.h>
#include <texture_registry.h>

FILE_DESCR("camerapi plugin")

enum data_source_type
{
    kDataSourceTypeAsset,
    kDataSourceTypeNetwork,
    kDataSourceTypeFile,
    kDataSourceTypeContentUri
};

struct camerapi_meta
{
    char *event_channel_name;

    // We have a listener to the video player event channel.
    bool has_listener;

    /*
    sd_event_source *probe_video_info_source;
    bool has_video_info;
    bool is_stream;
    int64_t duration_ms;
    int32_t width, height;
    */

    atomic_bool is_buffering;

    struct listener *video_info_listener;
    struct listener *buffering_state_listener;
};

static struct plugin
{
    struct flutterpi *flutterpi;
    bool initialized;
    struct concurrent_pointer_set players;
} plugin;

/// Add a player instance to the player collection.
static int add_player(struct camerapi *player)
{
    return cpset_put(&plugin.players, player);
}

/// Get a player instance by its id.
static struct camerapi *get_player_by_texture_id(int64_t texture_id)
{
    struct camerapi *player;

    cpset_lock(&plugin.players);
    for_each_pointer_in_cpset(&plugin.players, player)
    {
        if (camerapi_get_texture_id(player) == texture_id) {
            cpset_unlock(&plugin.players);
            return player;
        }
    }

    cpset_unlock(&plugin.players);
    return NULL;
}

/// Get a player instance by its event channel name.
static struct camerapi *get_player_by_evch(const char *const event_channel_name)
{
    struct camerapi_meta *meta;
    struct camerapi *player;

    cpset_lock(&plugin.players);
    for_each_pointer_in_cpset(&plugin.players, player)
    {
        meta = camerapi_get_userdata_locked(player);
        if (strcmp(meta->event_channel_name, event_channel_name) == 0) {
            cpset_unlock(&plugin.players);
            return player;
        }
    }

    cpset_unlock(&plugin.players);
    return NULL;
}

/// Remove a player instance from the player collection.
static int remove_player(struct camerapi *player)
{
    return cpset_remove(&plugin.players, player);
}

static struct camerapi_meta *get_meta(struct camerapi *player)
{
    return (struct camerapi_meta *)camerapi_get_userdata_locked(player);
}

/// Get the player id from the given arg, which is a kStdMap.
/// (*texture_id_out = arg['playerId'])
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int
get_texture_id_from_map_arg(struct std_value *arg, int64_t *texture_id_out, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct std_value *id;
    int ok;

    if (arg->type != kStdMap) {
        ok = platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg` to be a Map, but was: ", arg);
        if (ok != 0)
            return ok;

        return EINVAL;
    }

    id = stdmap_get_str(arg, "textureId");
    if (id == NULL || !STDVALUE_IS_INT(*id)) {
        ok = platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg['textureId']` to be an integer, but was: ", id);
        if (ok != 0)
            return ok;

        return EINVAL;
    }

    *texture_id_out = STDVALUE_AS_INT(*id);

    return 0;
}

/// Get the player associated with the id in the given arg, which is a kStdMap.
/// (*player_out = get_player_by_texture_id(get_texture_id_from_map_arg(arg)))
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int
get_player_from_map_arg(struct std_value *arg, struct camerapi **player_out, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    int64_t texture_id;
    int ok;

    texture_id = 0;
    ok = get_texture_id_from_map_arg(arg, &texture_id, responsehandle);
    if (ok != 0) {
        return ok;
    }

    player = get_player_by_texture_id(texture_id);
    if (player == NULL) {
        cpset_lock(&plugin.players);

        int n_texture_ids = cpset_get_count_pointers_locked(&plugin.players);
        int64_t *texture_ids = alloca(sizeof(int64_t) * n_texture_ids);
        int64_t *texture_ids_cursor = texture_ids;

        for_each_pointer_in_cpset(&plugin.players, player)
        {
            *texture_ids_cursor++ = camerapi_get_texture_id(player);
        }

        cpset_unlock(&plugin.players);

        ok = platch_respond_illegal_arg_ext_pigeon(
          responsehandle,
          "Expected `arg['textureId']` to be a valid texture id.",
          &STDMAP2(
            STDSTRING("textureId"),
            STDINT64(texture_id),
            STDSTRING("registeredTextureIds"),
            ((struct std_value){ .type = kStdInt64Array, .size = n_texture_ids, .int64array = texture_ids })));
        if (ok != 0)
            return ok;

        return EINVAL;
    }

    *player_out = player;

    return 0;
}

static int get_player_and_meta_from_map_arg(
  struct std_value *arg,
  struct camerapi **player_out,
  struct camerapi_meta **meta_out,
  FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return ok;
    }

    if (player_out) {
        *player_out = player;
    }

    if (meta_out) {
        *meta_out = (struct camerapi_meta *)camerapi_get_userdata_locked(player);
    }

    return 0;
}

static int ensure_initialized()
{
    GError *gst_error;
    gboolean success;

    if (plugin.initialized) {
        return 0;
    }

    success = gst_init_check(NULL, NULL, &gst_error);
    if (!success) {
        LOG_ERROR("Could not initialize gstreamer: %s\n", gst_error->message);
        return gst_error->code;
    }

    plugin.initialized = true;
    return 0;
}

static int respond_init_failed(FlutterPlatformMessageResponseHandle *handle)
{
    return platch_respond_error_pigeon(
      handle,
      "couldnotinit",
      "gstreamer video player plugin failed to initialize gstreamer. See flutter-pi log for details.",
      NULL);
}

static int send_initialized_event(struct camerapi_meta *meta, bool is_stream, int width, int height, int64_t duration_ms)
{
    return platch_send_success_event_std(
      meta->event_channel_name,
      &STDMAP4(
        STDSTRING("event"),
        STDSTRING("initialized"),
        STDSTRING("duration"),
        STDINT64(is_stream ? INT64_MAX : duration_ms),
        STDSTRING("width"),
        STDINT32(width),
        STDSTRING("height"),
        STDINT32(height)));
}

static int send_completed_event(struct camerapi_meta *meta)
{
    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("event"), STDSTRING("completed")));
}

static int send_buffering_update(struct camerapi_meta *meta, int n_ranges, const struct buffering_range *ranges)
{
    struct std_value values;

    values.type = kStdList;
    values.size = n_ranges;
    values.list = alloca(sizeof(struct std_value) * n_ranges);

    for (size_t i = 0; i < n_ranges; i++) {
        values.list[i].type = kStdList;
        values.list[i].size = 2;
        values.list[i].list = alloca(sizeof(struct std_value) * 2);

        values.list[i].list[0] = STDINT32(ranges[i].start_ms);
        values.list[i].list[1] = STDINT32(ranges[i].stop_ms);
    }

    return platch_send_success_event_std(
      meta->event_channel_name, &STDMAP2(STDSTRING("event"), STDSTRING("bufferingUpdate"), STDSTRING("values"), values));
}

static int send_buffering_start(struct camerapi_meta *meta)
{
    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("event"), STDSTRING("bufferingStart")));
}

static int send_buffering_end(struct camerapi_meta *meta)
{
    return platch_send_success_event_std(meta->event_channel_name, &STDMAP1(STDSTRING("event"), STDSTRING("bufferingEnd")));
}

static enum listener_return on_video_info_notify(void *arg, void *userdata)
{
    struct camerapi_meta *meta;
    struct video_info *info;

    DEBUG_ASSERT_NOT_NULL(userdata);
    meta = userdata;
    info = arg;

    // When the video info is not known yet, we still get informed about it.
    // In that case arg == NULL.
    if (arg == NULL) {
        return kNoAction;
    }

    LOG_DEBUG(
      "Got video info: stream? %s, w x h: % 4d x % 4d, duration: %" GST_TIME_FORMAT "\n",
      !info->can_seek ? "yes" : "no",
      info->width,
      info->height,
      GST_TIME_ARGS(info->duration_ms * GST_MSECOND));

    /// on_video_info_notify is called on an internal thread,
    /// but send_initialized_event is (should be) mt-safe
    send_initialized_event(meta, !info->can_seek, info->width, info->height, info->duration_ms);

    /// TODO: We should only send the initialized event once,
    /// but maybe it's also okay if we send it multiple times?
    return kUnlisten;
}

static enum listener_return on_buffering_state_notify(void *arg, void *userdata)
{
    struct buffering_state *state;
    struct camerapi_meta *meta;
    bool new_is_buffering;

    DEBUG_ASSERT_NOT_NULL(userdata);
    meta = userdata;
    state = arg;

    if (arg == NULL) {
        return kNoAction;
    }

    new_is_buffering = state->percent != 100;

    if (meta->is_buffering && !new_is_buffering) {
        send_buffering_end(meta);
        meta->is_buffering = false;
    } else if (!meta->is_buffering && new_is_buffering) {
        send_buffering_start(meta);
        meta->is_buffering = true;
    }

    send_buffering_update(meta, state->n_ranges, state->ranges);
    return kNoAction;
}

/*******************************************************
 * CHANNEL HANDLERS                                    *
 * handle method calls on the method and event channel *
 *******************************************************/
static int on_receive_evch(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi_meta *meta;
    struct camerapi *player;
    const char *method;

    method = object->method;

    LOG_DEBUG("on_receive_evch\n");

    player = get_player_by_evch(channel);
    if (player == NULL) {
        return platch_respond_not_implemented(responsehandle);
    }

    meta = camerapi_get_userdata_locked(player);

    if STREQ ("listen", method) {
        platch_respond_success_std(responsehandle, NULL);
        meta->has_listener = true;

        meta->video_info_listener = notifier_listen(camerapi_get_video_info_notifier(player), on_video_info_notify, NULL, meta);
        if (meta->video_info_listener == NULL) {
            LOG_ERROR("Couldn't listen for video info events in camerapi.\n");
        }

        meta->buffering_state_listener =
          notifier_listen(camerapi_get_buffering_state_notifier(player), on_buffering_state_notify, NULL, meta);
        if (meta->buffering_state_listener == NULL) {
            LOG_ERROR("Couldn't listen for buffering events in camerapi.\n");
        }
    } else if STREQ ("cancel", method) {
        platch_respond_success_std(responsehandle, NULL);
        meta->has_listener = false;

        if (meta->video_info_listener != NULL) {
            notifier_unlisten(camerapi_get_video_info_notifier(player), meta->video_info_listener);
            meta->video_info_listener = NULL;
        }
        if (meta->buffering_state_listener != NULL) {
            notifier_unlisten(camerapi_get_buffering_state_notifier(player), meta->buffering_state_listener);
            meta->buffering_state_listener = NULL;
        }
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return 0;
}

static int on_initialize(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    int ok;

    (void)channel;
    (void)object;

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    LOG_DEBUG("on_initialize\n");

    // what do we even do here?

    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int check_headers(const struct std_value *headers, FlutterPlatformMessageResponseHandle *responsehandle)
{
    const struct std_value *key, *value;

    if (headers == NULL || STDVALUE_IS_NULL(*headers)) {
        return 0;
    } else if (headers->type != kStdMap) {
        platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['httpHeaders']` to be a map of strings or null.");
        return EINVAL;
    }

    for (int i = 0; i < headers->size; i++) {
        key = headers->keys + i;
        value = headers->values + i;

        if (STDVALUE_IS_NULL(*key) || STDVALUE_IS_NULL(*value)) {
            // ignore this value
            continue;
        } else if (STDVALUE_IS_STRING(*key) && STDVALUE_IS_STRING(*value)) {
            // valid too
            continue;
        } else {
            platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['httpHeaders']` to be a map of strings or null.");
            return EINVAL;
        }
    }

    return 0;
}

static int add_headers_to_player(const struct std_value *headers, struct camerapi *player)
{
    const struct std_value *key, *value;

    if (headers == NULL || STDVALUE_IS_NULL(*headers)) {
        return 0;
    } else if (headers->type != kStdMap) {
        DEBUG_ASSERT(false);
    }

    for (int i = 0; i < headers->size; i++) {
        key = headers->keys + i;
        value = headers->values + i;

        if (STDVALUE_IS_NULL(*key) || STDVALUE_IS_NULL(*value)) {
            // ignore this value
            continue;
        } else if (STDVALUE_IS_STRING(*key) && STDVALUE_IS_STRING(*value)) {
            camerapi_put_http_header(player, STDVALUE_AS_STRING(*key), STDVALUE_AS_STRING(*value));
        } else {
            DEBUG_ASSERT(false);
        }
    }

    return 0;
}

/// Allocates and initializes a camerapi_meta struct, which we
/// use to store additional information in a camerapi instance.
/// (The event channel name for that player)
static struct camerapi_meta *create_meta(int64_t texture_id)
{
    struct camerapi_meta *meta;
    char *event_channel_name;

    meta = malloc(sizeof *meta);
    if (meta == NULL) {
        return NULL;
    }

    asprintf(&event_channel_name, "flutter.io/camerapi/videoEvents%" PRId64, texture_id);

    if (event_channel_name == NULL) {
        free(meta);
        return NULL;
    }

    meta->event_channel_name = event_channel_name;
    meta->has_listener = false;
    meta->is_buffering = false;
    return meta;
}

static void destroy_meta(struct camerapi_meta *meta)
{
    free(meta->event_channel_name);
    free(meta);
}

/// Creates a new video player.
/// Should respond to the platform message when the player has established its viewport.
static int on_create(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi_meta *meta;
    struct camerapi *player;
    struct std_value *arg, *temp;
    enum format_hint format_hint;
    char *asset, *uri, *package_name;
    int ok;

    (void)channel;

    arg = &(object->std_value);

    ok = ensure_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    if (!STDVALUE_IS_MAP(*arg)) {
        return platch_respond_illegal_arg_ext_pigeon(responsehandle, "Expected `arg` to be a Map, but was:", arg);
    }

    temp = stdmap_get_str(arg, "asset");
    if (temp == NULL || temp->type == kStdNull) {
        asset = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        asset = temp->string_value;
    } else {
        return platch_respond_illegal_arg_ext_pigeon(
          responsehandle, "Expected `arg['asset']` to be a String or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "uri");
    if (temp == NULL || temp->type == kStdNull) {
        uri = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        uri = temp->string_value;
    } else {
        return platch_respond_illegal_arg_ext_pigeon(
          responsehandle, "Expected `arg['uri']` to be a String or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "packageName");
    if (temp == NULL || temp->type == kStdNull) {
        package_name = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        package_name = temp->string_value;
    } else {
        return platch_respond_illegal_arg_ext_pigeon(
          responsehandle, "Expected `arg['packageName']` to be a String or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "formatHint");
    if (temp == NULL || temp->type == kStdNull) {
        format_hint = kNoFormatHint;
    } else if (temp != NULL && temp->type == kStdString) {
        char *format_hint_str = temp->string_value;

        if STREQ ("ss", format_hint_str) {
            format_hint = kSS_FormatHint;
        } else if STREQ ("hls", format_hint_str) {
            format_hint = kHLS_FormatHint;
        } else if STREQ ("dash", format_hint_str) {
            format_hint = kMpegDash_FormatHint;
        } else if STREQ ("other", format_hint_str) {
            format_hint = kOther_FormatHint;
        } else {
            goto invalid_format_hint;
        }
    } else {
    invalid_format_hint:

        return platch_respond_illegal_arg_ext_pigeon(
          responsehandle, "Expected `arg['formatHint']` to be one of 'ss', 'hls', 'dash', 'other' or null, but was:", temp);
    }

    temp = stdmap_get_str(arg, "httpHeaders");

    // check our headers are valid, so we don't create our player for nothing
    ok = check_headers(temp, responsehandle);
    if (ok != 0) {
        return 0;
    }

    // create our actual player (this doesn't initialize it)
    if (asset != NULL) {
        player = camerapi_new_from_asset(&flutterpi, asset, package_name, NULL);
    } else {
        player = camerapi_new_from_network(&flutterpi, uri, format_hint, NULL);
    }
    if (player == NULL) {
        LOG_ERROR("Couldn't create gstreamer video player.\n");
        ok = EIO;
        goto fail_respond_error;
    }

    // create a meta object so we can store the event channel name
    // of a player with it
    meta = create_meta(camerapi_get_texture_id(player));
    if (meta == NULL) {
        ok = ENOMEM;
        goto fail_destroy_player;
    }

    camerapi_set_userdata_locked(player, meta);

    // Add all our HTTP headers to camerapi using camerapi_put_http_header
    add_headers_to_player(temp, player);

    // add it to our player collection
    ok = add_player(player);
    if (ok != 0) {
        goto fail_destroy_meta;
    }

    // set a receiver on the videoEvents event channel
    ok = plugin_registry_set_receiver(meta->event_channel_name, kStandardMethodCall, on_receive_evch);
    if (ok != 0) {
        goto fail_remove_player;
    }

    // Finally, start initializing
    ok = camerapi_initialize(player);
    if (ok != 0) {
        goto fail_remove_receiver;
    }

    LOG_DEBUG("respond success on_create\n");

    return platch_respond_success_pigeon(
      responsehandle, &STDMAP1(STDSTRING("textureId"), STDINT64(camerapi_get_texture_id(player))));

fail_remove_receiver:
    plugin_registry_remove_receiver(meta->event_channel_name);

fail_remove_player:
    remove_player(player);

fail_destroy_meta:
    destroy_meta(meta);

fail_destroy_player:
    camerapi_destroy(player);

fail_respond_error:
    return platch_respond_native_error_pigeon(responsehandle, ok);
}

static int on_dispose(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi_meta *meta;
    struct camerapi *player;
    struct std_value *arg;
    int ok;

    (void)channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    meta = get_meta(player);

    plugin_registry_remove_receiver(meta->event_channel_name);

    remove_player(player);
    if (meta->video_info_listener != NULL) {
        notifier_unlisten(camerapi_get_video_info_notifier(player), meta->video_info_listener);
        meta->video_info_listener = NULL;
    }
    if (meta->buffering_state_listener != NULL) {
        notifier_unlisten(camerapi_get_buffering_state_notifier(player), meta->buffering_state_listener);
        meta->buffering_state_listener = NULL;
    }
    destroy_meta(meta);
    camerapi_destroy(player);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

/* static int on_set_looping(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) */
/* { */
/*     struct camerapi *player; */
/*     struct std_value *arg, *temp; */
/*     bool loop; */
/*     int ok; */

/*     (void)channel; */

/*     arg = &object->std_value; */

/*     ok = get_player_from_map_arg(arg, &player, responsehandle); */
/*     if (ok != 0) */
/*         return ok; */

/*     temp = stdmap_get_str(arg, "isLooping"); */
/*     if (temp && STDVALUE_IS_BOOL(*temp)) { */
/*         loop = STDVALUE_AS_BOOL(*temp); */
/*     } else { */
/*         return platch_respond_illegal_arg_ext_pigeon( */
/*           responsehandle, "Expected `arg['isLooping']` to be a boolean, but was:", temp); */
/*     } */

/*     camerapi_set_looping(player, loop); */
/*     return platch_respond_success_pigeon(responsehandle, NULL); */
/* } */

static int on_set_volume(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    struct std_value *arg, *temp;
    double volume;
    int ok;

    (void)channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return ok;

    temp = stdmap_get_str(arg, "volume");
    if (STDVALUE_IS_FLOAT(*temp)) {
        volume = STDVALUE_AS_FLOAT(*temp);
    } else {
        return platch_respond_illegal_arg_ext_pigeon(
          responsehandle, "Expected `arg['volume']` to be a float/double, but was:", temp);
    }

    camerapi_set_volume(player, volume);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

/* static int on_set_playback_speed(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle
 * *responsehandle) */
/* { */
/*     struct camerapi *player; */
/*     struct std_value *arg, *temp; */
/*     double speed; */
/*     int ok; */

/*     (void)channel; */

/*     arg = &object->std_value; */

/*     ok = get_player_from_map_arg(arg, &player, responsehandle); */
/*     if (ok != 0) */
/*         return ok; */

/*     temp = stdmap_get_str(arg, "speed"); */
/*     if (STDVALUE_IS_FLOAT(*temp)) { */
/*         speed = STDVALUE_AS_FLOAT(*temp); */
/*     } else { */
/*         return platch_respond_illegal_arg_ext_pigeon( */
/*           responsehandle, "Expected `arg['speed']` to be a float/double, but was:", temp); */
/*     } */

/*     camerapi_set_playback_speed(player, speed); */
/*     return platch_respond_success_pigeon(responsehandle, NULL); */
/* } */

static int on_play(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    struct std_value *arg;
    int ok;
    LOG_DEBUG("on_play()\n");

    (void)channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    camerapi_play(player);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_get_position(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    struct std_value *arg;
    int64_t position;
    int ok;

    (void)channel;
    (void)position;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    position = camerapi_get_position(player);

    if (position >= 0) {
        return platch_respond_success_pigeon(responsehandle, &STDMAP1(STDSTRING("position"), STDINT64(position)));
    } else {
        return platch_respond_error_pigeon(responsehandle, "native-error", "An unexpected gstreamer error ocurred.", NULL);
    }
}

static int on_seek_to(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    struct std_value *arg, *temp;
    int64_t position;
    int ok;

    (void)channel;

    arg = &(object->std_value);

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    temp = stdmap_get_str(arg, "position");
    if (STDVALUE_IS_INT(*temp)) {
        position = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['position']` to be an integer.");
    }

    camerapi_seek_to(player, position, false);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_pause(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    struct std_value *arg;
    int ok;

    (void)channel;

    arg = &object->std_value;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0)
        return 0;

    camerapi_pause(player);
    return platch_respond_success_pigeon(responsehandle, NULL);
}

static int on_set_mix_with_others(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct std_value *arg;

    (void)channel;

    arg = &object->std_value;

    (void)arg;

    /// TODO: Should we do anything other here than just returning?
    LOG_DEBUG("on_set_mix_with_others\n");
    return platch_respond_success_std(responsehandle, &STDNULL);
}

static int on_step_forward(struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    ok = camerapi_step_forward(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int on_step_backward(struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    ok = camerapi_step_backward(player);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int on_fast_seek(struct std_value *arg, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct camerapi *player;
    struct std_value *temp;
    int64_t position;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return 0;
    }

    temp = stdmap_get_str(arg, "position");
    if (STDVALUE_IS_INT(*temp)) {
        position = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_pigeon(responsehandle, "Expected `arg['position']` to be an integer.");
    }

    ok = camerapi_seek_to(player, position, true);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, NULL);
}

static int
on_receive_method_channel(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    const char *method;

    (void)channel;

    method = object->method;

    if STREQ ("stepForward", method) {
        return on_step_forward(&object->std_arg, responsehandle);
    } else if STREQ ("stepBackward", method) {
        return on_step_backward(&object->std_arg, responsehandle);
    } else if STREQ ("fastSeek", method) {
        return on_fast_seek(&object->std_arg, responsehandle);
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

enum plugin_init_result camerapi_plugin_init(struct flutterpi *flutterpi, void **userdata_out)
{
    int ok;

    (void)userdata_out;

    plugin.flutterpi = flutterpi;
    plugin.initialized = false;

    ok = cpset_init(&plugin.players, CPSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        return kError_PluginInitResult;
    }

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.initialize", kStandardMessageCodec, on_initialize);
    if (ok != 0) {
        goto fail_deinit_cpset;
    }

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.create", kStandardMessageCodec, on_create);
    if (ok != 0) {
        goto fail_remove_initialize_receiver;
    }

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.dispose", kStandardMessageCodec, on_dispose);
    if (ok != 0) {
        goto fail_remove_create_receiver;
    }

    /* ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.setLooping", kStandardMessageCodec, on_set_looping); */
    /* if (ok != 0) { */
    /*     goto fail_remove_dispose_receiver; */
    /* } */

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.setVolume", kStandardMessageCodec, on_set_volume);
    if (ok != 0) {
        goto fail_remove_setLooping_receiver;
    }

    /* ok = plugin_registry_set_receiver( */
    /*   "dev.flutter.pigeon.CameraPiApi.setPlaybackSpeed", kStandardMessageCodec, on_set_playback_speed); */
    /* if (ok != 0) { */
    /*     goto fail_remove_setVolume_receiver; */
    /* } */

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.play", kStandardMessageCodec, on_play);
    if (ok != 0) {
        goto fail_remove_setPlaybackSpeed_receiver;
    }

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.position", kStandardMessageCodec, on_get_position);
    if (ok != 0) {
        goto fail_remove_play_receiver;
    }

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.seekTo", kStandardMessageCodec, on_seek_to);
    if (ok != 0) {
        goto fail_remove_position_receiver;
    }

    ok = plugin_registry_set_receiver("dev.flutter.pigeon.CameraPiApi.pause", kStandardMessageCodec, on_pause);
    if (ok != 0) {
        goto fail_remove_seekTo_receiver;
    }

    ok = plugin_registry_set_receiver(
      "dev.flutter.pigeon.CameraPiApi.setMixWithOthers", kStandardMessageCodec, on_set_mix_with_others);
    if (ok != 0) {
        goto fail_remove_pause_receiver;
    }

    ok = plugin_registry_set_receiver(
      "flutter.io/videoPlayer/gstreamerVideoPlayer/advancedControls", kStandardMethodCall, on_receive_method_channel);
    if (ok != 0) {
        goto fail_remove_setMixWithOthers_receiver;
    }

    return kInitialized_PluginInitResult;

fail_remove_setMixWithOthers_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setMixWithOthers");

fail_remove_pause_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.pause");

fail_remove_seekTo_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.seekTo");

fail_remove_position_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.position");

fail_remove_play_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.play");

fail_remove_setPlaybackSpeed_receiver:
    /* plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setPlaybackSpeed"); */

fail_remove_setVolume_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setVolume");

fail_remove_setLooping_receiver:
    /* plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setLooping"); */

fail_remove_dispose_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.dispose");

fail_remove_create_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.create");

fail_remove_initialize_receiver:
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.initialize");

fail_deinit_cpset:
    cpset_deinit(&plugin.players);
    return kError_PluginInitResult;
}

void camerapi_plugin_deinit(struct flutterpi *flutterpi, void *userdata)
{
    (void)flutterpi;
    (void)userdata;

    plugin_registry_remove_receiver("flutter.io/videoPlayer/gstreamerVideoPlayer/advancedControls");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setMixWithOthers");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.pause");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.seekTo");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.position");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.play");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setPlaybackSpeed");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setVolume");
    /* plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.setLooping"); */
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.dispose");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.create");
    plugin_registry_remove_receiver("dev.flutter.pigeon.CameraPiApi.initialize");
    cpset_deinit(&plugin.players);
}

FLUTTERPI_PLUGIN("CameraPi", camerapi, camerapi_plugin_init, camerapi_plugin_deinit)
