// libdbapiaamod.so — jciDbapi plugin backing the head unit's Settings screen.
//
// The Opera GUI has no filesystem access, only its three WebSockets. jciDbapi is
// the OEM's own extension point for that: registering one plugin here lets the
// Settings screen edit libpatch.conf with no extra daemon, socket or shell glue.
// See dbapi_plugin.h for the host ABI.
//
//   GetConfig {}             -> the exposed schema with current values
//   SetConfig {key, value}   -> validate against that schema, rewrite that key
//
// GetConfig returns the schema, not just values, so the GUI renders whatever it is
// handed and never names a key: exposing a new setting is a change to kSchema and
// nothing else.
//
// `label` is the ENGLISH FALLBACK, not display text — the screen is translated and
// those strings live GUI-side in aaModSettings.js, keyed by config key. A key with
// no translation falls back to the label here.
//
// Values are *effective*: config.h's defaults overridden by libpatch.conf, exactly
// as each shim reads them, so the screen shows what the mod is really doing even
// for keys the file omits.
//
// Only booleans and small enums are editable. A number is shown read-only (an
// "info" row). Free-text keys stay file-only — they would need the OEM keyboard
// on a second borrowed MMUI context, and a typo'd passphrase entered on a
// touchscreen is worse than editing the file over ssh.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // dladdr() declaration pulled in by common/config.h
#endif

#define LOG_TAG "AAMOD"
#include "log.h"

#include "common/config.h"
#include "conf_write.h"
#include "dbapi_plugin.h"
#include "jansson_rt.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace {

// Must equal the `name` in /jci/dbapi/plugins.xml — the host also builds our
// filename from it: /jci/dbapi/plugins/libdbapi<name>.so.
const char *const kApiName = "aamod";

// Stated explicitly: the plugin lives in /jci/dbapi/plugins/, not next to the
// shims, so config.h's dladdr-based find_sibling_file() cannot find the config.
// Override with -DAAMOD_CONF_PATH='"..."'.
#ifndef AAMOD_CONF_PATH
#  define AAMOD_CONF_PATH "/data_persist/oem-aa-mod/libpatch.conf"
#endif

using libpatch_config::Settings;

// ---------------------------------------------------------------------------
// Exposed schema
// ---------------------------------------------------------------------------

// Written to libpatch.conf verbatim, so they must be spellings apply_kv() accepts.
const char *const kBoolTrue  = "true";
const char *const kBoolFalse = "false";

// hud_transport. config.h also accepts "svcjcinavi" for "svcnavi"; we write the
// short spelling and reading is alias-tolerant via apply_kv().
//
// The labels ARE the values: config tokens naming the two HUD backends, shown
// verbatim in every language. The GUI has no string-table entry for them, which is
// what makes them untranslatable rather than merely discouraged.
const char *const kTransportValues[] = { "svcnavi", "vbs", NULL };
const char *const kTransportLabels[] = { "svcnavi", "vbs", NULL };

// INFO rows are shown, never written: value_allowed() rejects every value for
// them, so SetConfig cannot touch the key.
enum Kind { KIND_BOOL, KIND_ENUM, KIND_INFO };
const char *const kKindNames[] = { "bool", "enum", "info" };   // index = Kind

struct Item {
    const char *key;
    const char *label;              // English fallback
    Kind        kind;
    const char *const *values;      // enum only, NULL-terminated, index-parallel
    const char *const *labels;      // with values: what the row's buttons read
    const char *(*get)(const Settings &s);   // effective value as a canonical string
};

// Keeps each bool entry to one line.
#define BOOL_GET(field)                                                        \
    [](const Settings &s) -> const char * {                                    \
        return s.field ? kBoolTrue : kBoolFalse;                               \
    }

const Item kSchema[] = {
    // --- blmjciaapa: input + HUD --------------------------------------------
    { "touch", "Touch passthrough", KIND_BOOL, NULL, NULL, BOOL_GET(touch) },
    { "hud", "HUD guidance", KIND_BOOL, NULL, NULL, BOOL_GET(hud) },
    { "hud_transport", "HUD transport", KIND_ENUM, kTransportValues, kTransportLabels,
      [](const Settings &s) -> const char * {
          return s.hud_transport == libpatch_config::HUD_TRANSPORT_SVCNAVI
                     ? kTransportValues[0]
                     : kTransportValues[1];
      } },
    { "force_street_name", "HUD street name", KIND_BOOL, NULL, NULL,
      BOOL_GET(force_street_name) },
    { "hud_fold_latin", "HUD accent folding", KIND_BOOL, NULL, NULL,
      BOOL_GET(hud_fold_latin) },

    // --- Android Auto protocol ---------------------------------------------
    { "use_protocol_v1_6", "AA protocol 1.6", KIND_BOOL, NULL, NULL,
      BOOL_GET(use_protocol_v1_6) },
    // Only does anything with use_protocol_v1_6 = true.
    { "roundabout_prepend_exit_number", "Roundabout exit number", KIND_BOOL, NULL,
      NULL, BOOL_GET(roundabout_prepend_exit_number) },

    // Read-only: a free number, which no toggle style can enter. Sent as plain
    // metres, 0 = always shown; the GUI formats it.
    { "hud_maneuver_max_distance_m", "Show HUD maneuver from", KIND_INFO, NULL,
      NULL,
      [](const Settings &s) -> const char * {
          static char buf[16];
          snprintf(buf, sizeof(buf), "%u",
                   static_cast<unsigned>(s.hud_maneuver_max_distance_m));
          return buf;
      } },

    // --- svcjcinavi: cluster ------------------------------------------------
    { "compass_always_on", "Compass always on", KIND_BOOL, NULL, NULL,
      BOOL_GET(compass_always_on) },

    // --- GAL 1.6 pairing workaround ----------------------------------------
    // Both only do anything with use_protocol_v1_6 = true.
    { "bt_pairing_bypass_all_devices", "1.6 bypass: all devices", KIND_BOOL, NULL,
      NULL, BOOL_GET(bt_pairing_bypass_all_devices) },
    { "bt_pairing_show_device_notification", "Show USB device name", KIND_BOOL, NULL,
      NULL, BOOL_GET(bt_pairing_show_device_notification) },

    // --- audio -------------------------------------------------------------
    // No separate entry for the tail-cutoff or beginning-cutoff (dmix keep-alive)
    // fixes: neither is a key of its own — both ride on
    // aa_audio_low_latency together with the head-cutoff half.
    { "mute_pauses_phone", "Mute pauses phone", KIND_BOOL, NULL, NULL,
      BOOL_GET(mute_pauses_phone) },
    { "unmute_starts_playback", "Unmute starts playback", KIND_BOOL, NULL, NULL,
      BOOL_GET(unmute_starts_playback) },
    { "block_headunit_media_play", "Block unit play", KIND_BOOL, NULL, NULL,
      BOOL_GET(block_headunit_media_play) },
    { "aa_audio_low_latency", "Low-latency audio", KIND_BOOL, NULL, NULL,
      BOOL_GET(aa_audio_low_latency) },
};

#undef BOOL_GET

const size_t kSchemaCount = sizeof(kSchema) / sizeof(kSchema[0]);

const Item *find_item(const char *key)
{
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < kSchemaCount; ++i) {
        if (strcasecmp(kSchema[i].key, key) == 0) {
            return &kSchema[i];
        }
    }
    return NULL;
}

// True if `value` is one the item accepts. Nothing else may ever be written.
bool value_allowed(const Item &item, const char *value)
{
    if (value == NULL) {
        return false;
    }
    if (item.kind == KIND_INFO) {
        return false;
    }
    if (item.kind == KIND_BOOL) {
        return strcmp(value, kBoolTrue) == 0 || strcmp(value, kBoolFalse) == 0;
    }
    for (size_t i = 0; item.values[i] != NULL; ++i) {
        if (strcmp(item.values[i], value) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Host wiring
// ---------------------------------------------------------------------------

dbapi::MethodCb g_method_cb = NULL;

// Built-in defaults overridden by libpatch.conf. `present` reports whether the
// file was readable, so the screen can say "showing defaults" honestly.
Settings read_effective(bool *present)
{
    Settings s;   // config.h's compiled-in defaults
    const bool ok =
        libpatch_config::parse_file(AAMOD_CONF_PATH, &libpatch_config::apply_kv, &s);
    if (present != NULL) {
        *present = ok;
    }
    if (!ok) {
        LOGW("GetConfig: %s not readable — reporting built-in defaults",
             AAMOD_CONF_PATH);
    }
    return s;
}

// Hand `params` to the host and drop our reference; the host took its own.
void respond(int reqId, const char *methodName, json_t *params)
{
    if (g_method_cb == NULL) {
        LOGE("%s: no method callback — dropping response", methodName);
        jansson::decref(params);
        return;
    }
    g_method_cb(kApiName, reqId, methodName, params);
    jansson::decref(params);
}

// One failure shape, so the GUI branches on one field.
void respond_error(int reqId, const char *methodName, const char *why)
{
    const jansson::Api *j = jansson::api();
    if (j == NULL) {
        LOGE("%s: %s (and no jansson — cannot even report it)", methodName, why);
        return;
    }
    LOGW("%s: %s", methodName, why);

    json_t *root = j->object();
    if (root == NULL) {
        return;
    }
    j->object_set_new(root, "result", j->integer(1));
    j->object_set_new(root, "error", j->string(why));
    respond(reqId, methodName, root);
}

// GetConfig {} -> { result, path, present, items:[{key,label,type,value[,options,labels]}] }
void GetConfig(int reqId, json_t * /*params*/)
{
    const jansson::Api *j = jansson::api();
    if (j == NULL) {
        return;   // api() already logged; without jansson we cannot reply at all
    }

    bool           present = false;
    const Settings s       = read_effective(&present);

    json_t *root  = j->object();
    json_t *items = j->array();
    if (root == NULL || items == NULL) {
        jansson::decref(root);
        jansson::decref(items);
        LOGE("GetConfig: out of memory building response");
        return;
    }

    for (size_t i = 0; i < kSchemaCount; ++i) {
        const Item &it  = kSchema[i];
        json_t     *obj = j->object();
        if (obj == NULL) {
            break;
        }
        j->object_set_new(obj, "key", j->string(it.key));
        j->object_set_new(obj, "label", j->string(it.label));
        j->object_set_new(obj, "type", j->string(kKindNames[it.kind]));
        j->object_set_new(obj, "value", j->string(it.get(s)));

        if (it.kind == KIND_ENUM) {
            json_t *vals = j->array();
            json_t *labs = j->array();
            for (size_t k = 0; it.values[k] != NULL; ++k) {
                j->array_append_new(vals, j->string(it.values[k]));
                j->array_append_new(labs, j->string(it.labels[k]));
            }
            j->object_set_new(obj, "options", vals);
            j->object_set_new(obj, "labels", labs);
        }
        j->array_append_new(items, obj);
    }

    j->object_set_new(root, "result", j->integer(0));
    j->object_set_new(root, "path", j->string(AAMOD_CONF_PATH));
    j->object_set_new(root, "present", present ? j->boolean_true()
                                               : j->boolean_false());
    j->object_set_new(root, "items", items);

    LOGD("GetConfig: %u items, conf %s", (unsigned)kSchemaCount,
         present ? "present" : "absent (defaults)");
    respond(reqId, "GetConfig", root);
}

// SetConfig {key, value} -> { result, key, value } / { result, error }
void SetConfig(int reqId, json_t *params)
{
    const jansson::Api *j = jansson::api();
    if (j == NULL) {
        return;
    }

    // The host already rejected a non-object params, so this is an object.
    const json_t *jkey = j->object_get(params, "key");
    const json_t *jval = j->object_get(params, "value");
    const char   *key  = jkey ? j->string_value(jkey) : NULL;
    const char   *val  = jval ? j->string_value(jval) : NULL;

    if (key == NULL || val == NULL) {
        respond_error(reqId, "SetConfig", "params need string key and value");
        return;
    }

    const Item *item = find_item(key);
    if (item == NULL) {
        // Refusing keys outside the schema is what stops the GUI writing
        // arbitrary config.
        respond_error(reqId, "SetConfig", "key is not exposed by this screen");
        return;
    }
    if (!value_allowed(*item, val)) {
        respond_error(reqId, "SetConfig", "value not valid for this key");
        return;
    }

    if (!conf_write::set_key(AAMOD_CONF_PATH, item->key, val)) {
        respond_error(reqId, "SetConfig", "could not write libpatch.conf");
        return;
    }

    json_t *root = j->object();
    if (root == NULL) {
        return;
    }
    j->object_set_new(root, "result", j->integer(0));
    j->object_set_new(root, "key", j->string(item->key));
    j->object_set_new(root, "value", j->string(val));

    LOGD("SetConfig: %s = %s", item->key, val);
    respond(reqId, "SetConfig", root);
}

// The table the host walks in callMethod(). NULL-terminated, {name, fn} pairs.
const dbapi::Method kMethods[] = {
    { "GetConfig", &GetConfig },
    { "SetConfig", &SetConfig },
    { NULL, NULL },
};

} // namespace

// ---------------------------------------------------------------------------
// Plugin entry points
// ---------------------------------------------------------------------------

// Signature per dbapi_plugin.h. We spawn no threads, so the watchdog helpers are
// named for documentation only.
extern "C" __attribute__((visibility("default")))
int DBAPI_PluginInit(const dbapi::Method **out_methods,
                     dbapi::MethodCb       method_cb,
                     dbapi::WdmCreateSubscriberFn /*wdm_create_subscriber*/,
                     dbapi::WdmSubscribeFn        /*wdm_subscribe*/,
                     dbapi::WdmUnsubscribeFn      /*wdm_unsubscribe*/,
                     dbapi::WdmTickleFn           /*wdm_tickle*/,
                     dbapi::WdmLocalTickleFn      /*wdm_local_tickle*/)
{
    if (out_methods != NULL) {
        *out_methods = kMethods;
    }

    if (method_cb == NULL) {
        // No way to answer: refuse to come up, as the OEM plugins do.
        LOGE("init: host passed no method callback");
        return dbapi::kInitFail;
    }
    g_method_cb = method_cb;

    // Bind now. Without jansson no method could ever answer, so refuse to come
    // up rather than sit loaded but mute: the host marks a plugin whose Init
    // fails as failed and carries on with the others.
    if (jansson::api() == NULL) {
        LOGE("init: jansson unavailable — refusing to initialise");
        g_method_cb = NULL;
        return dbapi::kInitFail;
    }

    LOGD("init: api=\"%s\" conf=%s methods=%u", kApiName, AAMOD_CONF_PATH,
         (unsigned)(sizeof(kMethods) / sizeof(kMethods[0]) - 1));
    return dbapi::kOk;
}

extern "C" __attribute__((visibility("default")))
void DBAPI_PluginDestroy(void)
{
    // Nothing is owned across calls: no threads, no cached json, config re-read
    // per GetConfig.
    g_method_cb = NULL;
}
