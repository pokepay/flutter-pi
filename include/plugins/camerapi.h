#ifndef _FLUTTERPI_INCLUDE_PLUGINS_CAMERAPI_PLUGIN_H
#define _FLUTTERPI_INCLUDE_PLUGINS_CAMERAPI_PLUGIN_H

#include "gstreamer_video_player.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <collection.h>

struct camera_video_info {
    int width, height;
    double fps;
};

struct camerapi;
struct flutterpi;

typedef struct BarcodeInfo {
    char *barcode;
    char *barcode_type;
    int quality;
} BarcodeInfo;

BarcodeInfo *BarcodeInfo_new(const char *barcode, const char *barcode_type, int quality);

void BarcodeInfo_destroy(BarcodeInfo *barcode_info);

/// Create a camerapi instance.
///     @arg userdata       The userdata associated with this player
struct camerapi *camerapi_new(struct flutterpi *flutterpi, void *userdata);

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
///     @arg userdata The new userdata that should be associated with this
///     player.
void camerapi_set_userdata_locked(struct camerapi *player, void *userdata);

/// Get the userdata that was given to the constructor or was previously set
/// using
/// @ref camerapi_set_userdata_locked.
///     @returns userdata associated with this player.
void *camerapi_get_userdata_locked(struct camerapi *player);

/// Get the id of the flutter external texture that this player is rendering
/// into.
int64_t camerapi_get_texture_id(struct camerapi *player);

// void camerapi_set_info_callback(struct camerapi *player,
// camerapi_info_callback_t cb, void *userdata);

// void camerapi_set_buffering_callback(struct camerapi *player,
// camerapi_buffering_callback_t callback, void *userdata);

/// Initializes the video playback, i.e. boots up the gstreamer pipeline, starts
/// buffering the video.
///     @returns 0 if initialization was successfull, errno-style error code if
///     an error ocurred.
int camerapi_initialize(struct camerapi *player);

/// Get the video info. If the video info (format, size, etc) is already known,
/// @arg callback will be called synchronously, inside this call. If the video
/// info is not known, @arg callback will be called on the flutter-pi platform
/// thread as soon as the info is known.
///     @returns The handle for the deferred callback.
// struct sd_event_source_generic *camerapi_probe_video_info(struct camerapi
// *player, camerapi_info_callback_t callback, void *userdata);

/// @brief Get the value notifier for the video info.
///
/// Gets notified with a value of type `struct video_info*` when the video info
/// changes. The listeners will be called on an internal gstreamer thread. So
/// you need to make sure you do the proper rethreading in the listener
/// callback.
struct notifier *camerapi_get_video_info_notifier(struct camerapi *player);

/// @brief Get the value notifier for the buffering state.
///
/// Gets notified with a value of type `struct buffering_state*` when the
/// buffering state changes. The listeners will be called on the main flutterpi
/// platform thread.
struct notifier *camerapi_get_buffering_state_notifier(struct camerapi *player);

/// @brief Get the change notifier for errors.
///
/// Gets notified when an error happens. (Not yet implemented)
struct notifier *camerapi_get_error_notifier(struct camerapi *player);

/// @brief Get the value notifier for barcodes
///
/// Gets notified when a barcode is scanned.
struct notifier *camerapi_barcode_notifier(struct camerapi *player);

#endif
