#ifndef _FLUTTERPI_INCLUDE_PLUGINS_CAMERAPI_PLUGIN_H
#define _FLUTTERPI_INCLUDE_PLUGINS_CAMERAPI_PLUGIN_H

#include "gstreamer_video_player.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <collection.h>

struct camerapi;
struct flutterpi;

/// Create a gstreamer video player that loads the video from a flutter asset.
///     @arg asset_path     The path of the asset inside the asset bundle.
///     @arg package_name   The name of the package containing the asset
///     @arg userdata       The userdata associated with this player
struct camerapi *
camerapi_new_from_asset(struct flutterpi *flutterpi, const char *asset_path, const char *package_name, void *userdata);

/// Create a gstreamer video player that loads the video from a network URI.
///     @arg uri          The URI to the video. (for example, http://, https://, rtmp://, rtsp://)
///     @arg format_hint  A hint to the format of the video. kNoFormatHint means there's no hint.
///     @arg userdata     The userdata associated with this player.
struct camerapi *
camerapi_new_from_network(struct flutterpi *flutterpi, const char *uri, enum format_hint format_hint, void *userdata);

/// Create a gstreamer video player that loads the video from a file URI.
///     @arg uri        The file:// URI to the video.
///     @arg userdata   The userdata associated with this player.
struct camerapi *camerapi_new_from_file(struct flutterpi *flutterpi, const char *uri, void *userdata);

/// Destroy this gstreamer player instance and the resources
/// associated with it. (texture, gstreamer pipeline, etc)
///
/// Should be called on the flutterpi main/platform thread,
/// because otherwise destroying the gstreamer event bus listener
/// might be a race condition.
void camerapi_destroy(struct camerapi *player);

DECLARE_LOCK_OPS(camerapi)

/// Set the generic userdata associated with this gstreamer player instance.
/// Overwrites the userdata set in the constructor and any userdata previously
/// set using @ref camerapi_set_userdata_locked.
///     @arg userdata The new userdata that should be associated with this player.
void camerapi_set_userdata_locked(struct camerapi *player, void *userdata);

/// Get the userdata that was given to the constructor or was previously set using
/// @ref camerapi_set_userdata_locked.
///     @returns userdata associated with this player.
void *camerapi_get_userdata_locked(struct camerapi *player);

/// Get the id of the flutter external texture that this player is rendering into.
int64_t camerapi_get_texture_id(struct camerapi *player);

// void camerapi_set_info_callback(struct camerapi *player, camerapi_info_callback_t cb, void *userdata);

// void camerapi_set_buffering_callback(struct camerapi *player, camerapi_buffering_callback_t callback, void *userdata);

/// Add a http header (consisting of a string key and value) to the list of http headers that
/// gstreamer will use when playing back from a HTTP/S URI.
/// This has no effect after @ref camerapi_initialize was called.
void camerapi_put_http_header(struct camerapi *player, const char *key, const char *value);

/// Initializes the video playback, i.e. boots up the gstreamer pipeline, starts
/// buffering the video.
///     @returns 0 if initialization was successfull, errno-style error code if an error ocurred.
int camerapi_initialize(struct camerapi *player);

/// Get the video info. If the video info (format, size, etc) is already known, @arg callback will be called
/// synchronously, inside this call. If the video info is not known, @arg callback will be called on the flutter-pi
/// platform thread as soon as the info is known.
///     @returns The handle for the deferred callback.
// struct sd_event_source_generic *camerapi_probe_video_info(struct camerapi *player, camerapi_info_callback_t callback, void
// *userdata);

/// Set the current playback state to "playing" if that's not the case already.
///     @returns 0 if initialization was successfull, errno-style error code if an error ocurred.
int camerapi_play(struct camerapi *player);

/// Sets the current playback state to "paused" if that's not the case already.
///     @returns 0 if initialization was successfull, errno-style error code if an error ocurred.
int camerapi_pause(struct camerapi *player);

/// Get the current playback position.
///     @returns Current playback position, in milliseconds from the beginning of the video.
int64_t camerapi_get_position(struct camerapi *player);

/// Set whether the video should loop.
///     @arg looping    Whether the video should start playing from the beginning when the
///                     end is reached.
int camerapi_set_looping(struct camerapi *player, bool looping);

/// Set the playback volume.
///     @arg volume     Desired volume as a value between 0 and 1.
int camerapi_set_volume(struct camerapi *player, double volume);

/// Seek to a specific position in the video.
///     @arg position            Position to seek to in milliseconds from the beginning of the video.
///     @arg nearest_keyframe    If true, seek to the nearest keyframe instead. Might be faster but less accurate.
int camerapi_seek_to(struct camerapi *player, int64_t position, bool nearest_keyframe);

/// Set the playback speed of the player.
///   1.0: normal playback speed
///   0.5: half playback speed
///   2.0: double playback speed
int camerapi_set_playback_speed(struct camerapi *player, double playback_speed);

int camerapi_step_forward(struct camerapi *player);

int camerapi_step_backward(struct camerapi *player);

/// @brief Get the value notifier for the video info.
///
/// Gets notified with a value of type `struct video_info*` when the video info changes.
/// The listeners will be called on an internal gstreamer thread.
/// So you need to make sure you do the proper rethreading in the listener callback.
struct notifier *camerapi_get_video_info_notifier(struct camerapi *player);

/// @brief Get the value notifier for the buffering state.
///
/// Gets notified with a value of type `struct buffering_state*` when the buffering state changes.
/// The listeners will be called on the main flutterpi platform thread.
struct notifier *camerapi_get_buffering_state_notifier(struct camerapi *player);

/// @brief Get the change notifier for errors.
///
/// Gets notified when an error happens. (Not yet implemented)
struct notifier *camerapi_get_error_notifier(struct camerapi *player);

#endif
