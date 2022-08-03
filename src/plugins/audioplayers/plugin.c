#include <collection.h>
#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>

#define AUDIOPLAYERS_LOCAL_CHANNEL "xyz.luan/audioplayers"
#define AUDIOPLAYERS_GLOBAL_CHANNEL "xyz.luan/audioplayers.global"

static struct plugin
{
    struct flutterpi* flutterpi;
    bool initialized;
    struct concurrent_pointer_set players;
} plugin;

static int on_local_method_call(
  char* channel,
  struct platch_obj* object,
  FlutterPlatformMessageResponseHandle* responsehandle)
{
    const char* method;

    (void)channel;

    method = object->method;
    printf("[Audioplayers (local)] method call: %s\n", method);
    return 0;
}

static int on_global_method_call(
  char* channel,
  struct platch_obj* object,
  FlutterPlatformMessageResponseHandle* responsehandle)
{
    const char* method;

    (void)channel;

    method = object->method;
    printf("[Audioplayers (global)] method call: %s\n", method);
    return 0;
}

enum plugin_init_result audioplayers_plugin_init(struct flutterpi* flutterpi, void** userdata_out)
{
    printf("[Audioplayers] Init.\n");
    (void)userdata_out;
    int ok;
    plugin.flutterpi = flutterpi;
    plugin.initialized = false;

    ok = cpset_init(&plugin.players, CPSET_DEFAULT_MAX_SIZE);
    if (ok != 0)
        return kError_PluginInitResult;

    ok = plugin_registry_set_receiver(AUDIOPLAYERS_GLOBAL_CHANNEL, kStandardMethodCall, on_global_method_call);
    if (ok != 0) {
        goto fail_deinit_cpset;
    }

    ok = plugin_registry_set_receiver(AUDIOPLAYERS_LOCAL_CHANNEL, kStandardMethodCall, on_local_method_call);
    if (ok != 0) {
        goto fail_remove_global_receiver;
    }

    return kInitialized_PluginInitResult;

fail_remove_global_receiver:
    plugin_registry_remove_receiver(AUDIOPLAYERS_GLOBAL_CHANNEL);

fail_deinit_cpset:
    cpset_deinit(&plugin.players);
}

void audioplayers_plugin_deinit(struct flutterpi* flutterpi, void* userdata)
{
    (void)flutterpi;
    (void)userdata;
}

FLUTTERPI_PLUGIN(
  "audioplayers_flutter_pi",
  audioplayers,
  audioplayers_plugin_init,
  audioplayers_plugin_deinit)
