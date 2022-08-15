#include "camera_thread.h"
#include "collection.h"
#include "flutter-pi.h"
#include "flutter_embedder.h"
#include "mobile_scanner_plugin.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "texture_registry.h"
#include <pthread.h>
#include <stdint.h>

FILE_DESCR("MobileScanner")

#define MOBILE_SCANNER_METHOD_CHANNEL "dev.steenbakker.mobile_scanner/scanner/method"
#define MOBILE_SCANNER_EVENT_CHANNEL "dev.steenbakker.mobile_scanner/scanner/event"
#define ERROR_CODE "MobileScanner"

static struct plugin
{
    struct flutterpi *flutterpi;
    bool initialized;
    int64_t texture_id;
    pthread_t handle;
    CameraThreadState *thread_state;
} plugin;

// Prototypes
static int checkPermissions(FlutterPlatformMessageResponseHandle *responsehandle);
static int requestPermissions(FlutterPlatformMessageResponseHandle *responsehandle);
static int start(struct platch_obj *mcall, FlutterPlatformMessageResponseHandle *responsehandle);
static int toggleTorch(struct platch_obj *mcall, FlutterPlatformMessageResponseHandle *responsehandle);
static int stop(FlutterPlatformMessageResponseHandle *responsehandle);
static int analyzeImage(FlutterPlatformMessageResponseHandle *responsehandle);

static int on_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle)
{
    const char *method;
    (void)responsehandle;
    (void)channel;

    method = object->method;
    LOG_DEBUG("method call: %s\n", method);

    if (!strcmp(method, "state")) {
        return checkPermissions(responsehandle);
    } else if (!strcmp(method, "request")) {
        return requestPermissions(responsehandle);
    } else if (!strcmp(method, "start")) {
        return start(object, responsehandle);
    } else if (!strcmp(method, "torch")) {
        return toggleTorch(object, responsehandle);
    } else if (!strcmp(method, "stop")) {
        return stop(responsehandle);
    } else if (!strcmp(method, "analyzeImage")) {
        return analyzeImage(responsehandle);
    } else {
        LOG_ERROR("Unknown method: %s", method);
        return platch_respond_not_implemented(responsehandle);
    }
}

static int on_event(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle);

enum plugin_init_result mobile_scanner_plugin_init(struct flutterpi *flutterpi, void **userdata_out)
{
    LOG_DEBUG("init\n");
    (void)userdata_out;

    int ok;

    plugin.texture_id = 0;
    plugin.flutterpi = flutterpi;
    plugin.initialized = false;

    ok = plugin_registry_set_receiver(MOBILE_SCANNER_METHOD_CHANNEL, kStandardMethodCall, on_method_call);
    if (ok != 0)
        return kError_PluginInitResult;

    ok = plugin_registry_set_receiver(MOBILE_SCANNER_EVENT_CHANNEL, kStandardMessageCodec, on_event);
    if (ok != 0) {
        goto fail_remove_call_receiver;
    }

    plugin.thread_state = CameraThreadState_new();
    if (plugin.thread_state == NULL)
        goto fail_remove_event_receiver;

    CameraThreadResult res = CameraThreadState_init(plugin.thread_state, 0);
    if (res == CAMERA_THREAD_FAILURE)
        goto fail_free_thread_state;

    return kInitialized_PluginInitResult;

fail_free_thread_state:
    CameraThreadState_clean(plugin.thread_state);
    CameraThreadState_delete(plugin.thread_state);
    plugin.thread_state = NULL;
fail_remove_event_receiver:
    plugin_registry_remove_receiver(MOBILE_SCANNER_EVENT_CHANNEL);
fail_remove_call_receiver:
    plugin_registry_remove_receiver(MOBILE_SCANNER_METHOD_CHANNEL);

    return kError_PluginInitResult;
}

void mobile_scanner_plugin_deinit(struct flutterpi *flutterpi, void *userdata)
{
    (void)flutterpi;
    (void)userdata;
    // TODO
}

static int checkPermissions(FlutterPlatformMessageResponseHandle *responsehandle)
{
    // NOTE should we actually check for any kind of permissions here?
    int res = platch_respond_success_std(responsehandle, &STDINT64(1));
    LOG_DEBUG("state repond success\n");
    return res;
}

static int requestPermissions(FlutterPlatformMessageResponseHandle *responsehandle)
{
    return platch_respond_success_std(responsehandle, &STDBOOL(true));
}

static int start(struct platch_obj *mcall, FlutterPlatformMessageResponseHandle *responsehandle)
{
    struct texture *texture;
    int64_t texture_id;
    (void)mcall;

    LOG_DEBUG(">start()\n");

    texture = flutterpi_create_texture(plugin.flutterpi);
    if (texture == NULL) {
        LOG_ERROR("Could not create texture");
        return platch_respond_error_std(responsehandle, ERROR_CODE, "Could not create texture.", NULL);
    }
    LOG_DEBUG(">   getting texture id\n");
    texture_id = texture_get_id(texture);
    LOG_DEBUG(">   getting width and height id\n");
    double width = CameraThreadState_getWidth(plugin.thread_state);
    double height = CameraThreadState_getHeight(plugin.thread_state);
    LOG_DEBUG(">   preparing reply\n");
    struct std_value size = STDMAP2(STDSTRING("width"), STDFLOAT64(width), STDSTRING("height"), STDFLOAT64(height));

    /* pthread_create(&plugin.handle, 0, camera_thread_main, (void *)plugin.thread_state); */

    // clang-format off
    LOG_DEBUG(">   respond success\n");
    int res = platch_respond_success_std(
      responsehandle,
      &STDMAP3(STDSTRING("textureId"), STDINT64(texture_id), // --
               STDSTRING("size"),      size,
               STDSTRING("torchable"), STDBOOL(false)));
    // clang format on

    LOG_DEBUG(">   responded, ret = %d\n", res);
    return res;
}

static int toggleTorch(struct platch_obj *mcall, FlutterPlatformMessageResponseHandle *responsehandle)
{
    (void)mcall;
    return platch_respond_error_std(responsehandle, ERROR_CODE, "Cannot turn on torch. Operation not supported.", NULL);
}

static int stop(FlutterPlatformMessageResponseHandle *responsehandle)
{
        (void)responsehandle;

    // TODO
    return 0;
}

static int analyzeImage(FlutterPlatformMessageResponseHandle *responsehandle)
{
        (void)responsehandle;

    // TODO
    return 0;
}


static int on_event(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    // TODO
    /* platch_respond_success_std(responsehandle, struct std_value *return_value) */
    (void)channel;
    (void)object;
    (void)responsehandle;
    return 0;
}

FLUTTERPI_PLUGIN("mobile_scanner flutter-pi", mobile_scanner, mobile_scanner_plugin_init, mobile_scanner_plugin_deinit)
