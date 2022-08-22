/* #include "mobile_scanner.h" */
#include "camerapi.h"
#include "flutter_embedder.h"
#include "platformchannel.h"

#include <flutter-pi.h>
#include <notifier_listener.h>
#include <pluginregistry.h>

FILE_DESCR("mobile scanner plugin")

#define MOBILE_SCANNER_METHOD_CHANNEL "dev.steenbakker.mobile_scanner/scanner/method"
#define MOBILE_SCANNER_EVENT_CHANNEL "dev.steenbakker.mobile_scanner/scanner/event"

static struct plugin {
    struct camerapi *camerapi;
    struct flutterpi *flutterpi;
    struct listener *video_info_listener;
    struct listener *barcode_listener;
} plugin;

static int mobile_scanner_on_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle);

enum plugin_init_result mobile_scanner_plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    (void) userdata_out;
    int ok;

    plugin.flutterpi = flutterpi;

    ok = plugin_registry_set_receiver(MOBILE_SCANNER_METHOD_CHANNEL, kStandardMethodCall, mobile_scanner_on_method_call);
    if (ok != 0) {
        return kError_PluginInitResult;
    }

    // XXX What to do with this event channel?
    /* "dev.steenbakker.mobile_scanner/scanner/event" */
    /* ok = plugin_registry_set_receiver(const char *channel, enum platch_codec codec, platch_obj_recv_callback callback) */

    return kInitialized_PluginInitResult;
}

void mobile_scanner_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;
    plugin_registry_remove_receiver(MOBILE_SCANNER_METHOD_CHANNEL);
}

static enum listener_return mobile_scanner_on_video_info_notify(void *arg, void *userdata) {
    struct camera_video_info *info;
    FlutterPlatformMessageResponseHandle *responsehandle;

    DEBUG_ASSERT_NOT_NULL(userdata);
    info = arg;
    responsehandle = userdata;

    // When the video info is not known yet, we still get informed about it.
    // In that case arg == NULL.
    if (arg == NULL) {
        return kNoAction;
    }

    LOG_DEBUG("Got video info: w x h: % 4d x % 4d\n", info->width, info->height);

    /// on_video_info_notify is called on an internal thread,
    /// but send_initialized_event is (should be) mt-safe
    // clang-format off
    platch_respond_success_std(
        responsehandle,
        &STDMAP3(
            STDSTRING("textureId"), STDINT64(camerapi_get_texture_id(plugin.camerapi)),
            STDSTRING("size"),      STDMAP2(STDSTRING("width"),  STDFLOAT64(info->width),
                                            STDSTRING("height"), STDFLOAT64(info->height)),
            STDSTRING("torchable"), STDBOOL(false)
        )
    );
    // clang-format on

    /// TODO: We should only send the initialized event once,
    /// but maybe it's also okay if we send it multiple times?
    return kUnlisten;
}

static enum listener_return mobile_scanner_on_barcode_notify(void *arg, void *userdata) {
    struct barcode_info *info;
    char *data;
    char *type;
    (void) userdata;
    info = arg;

    if (arg == NULL) {
        return kNoAction;
    }

    data = malloc(strlen(info->barcode) + 1);
    strcpy(data, info->barcode);

    type = malloc(strlen(info->barcode_type) + 1);
    strcpy(type, info->barcode_type);

    // clang-format off
    platch_send_success_event_std(
        MOBILE_SCANNER_EVENT_CHANNEL,
        &STDMAP2(STDSTRING("name"), STDSTRING("barcode"),
                 STDSTRING("data"), STDSTRING(data))
    );
    // clang-format on

    return kNoAction;
}

static int mobile_scanner_on_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    const char *method;
    (void) channel;
    (void) responsehandle;

    method = object->method;
    if (!strcmp(method, "state")) {
        platch_respond_success_std(responsehandle, &STDINT64(1));
    } else if (!strcmp(method, "request")) {
        platch_respond_success_std(responsehandle, &STDBOOL(true));
    } else if (!strcmp(method, "start")) {
        if (plugin.camerapi != NULL) {
            LOG_ERROR("Camera already opened");
            return platch_respond_error_std(responsehandle, "MobileScanner", "camera already in use", NULL);
        }
        plugin.camerapi = camerapi_new(plugin.flutterpi, NULL);
        plugin.video_info_listener =
            notifier_listen(camerapi_get_video_info_notifier(plugin.camerapi), mobile_scanner_on_video_info_notify, NULL, responsehandle);
        plugin.barcode_listener =
            notifier_listen(camerapi_barcode_notifier(plugin.camerapi), mobile_scanner_on_barcode_notify, NULL, responsehandle);

        // clang-format off
        // clang-format on
    } else if (!strcmp(method, "stop")) {
        notifier_unlisten(camerapi_get_video_info_notifier(plugin.camerapi), plugin.video_info_listener);
        // NOTE should we somehow destroy the listener?
        plugin.video_info_listener = NULL;
        camerapi_destroy(plugin.camerapi);
        plugin.camerapi = NULL;
    } else if (!strcmp(method, "torch")) {
        platch_respond_error_std(responsehandle, "MobileScanner", "Torch not supported", NULL);
    } else if (!strcmp(method, "analyzeImage")) {
        platch_respond_not_implemented(responsehandle);
    }
    return platch_respond_not_implemented(responsehandle);
}

FLUTTERPI_PLUGIN("mobile_scanner", mobile_scanner, mobile_scanner_plugin_init, mobile_scanner_plugin_deinit);
