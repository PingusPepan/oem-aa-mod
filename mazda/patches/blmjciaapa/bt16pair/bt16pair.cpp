// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Wireless GAL-1.6 Bluetooth-pairing bypass. See bt16pair.h for the
// full rationale.

#define LOG_TAG "BT16PAIR"
#include "../log.h"
#include "bt16pair.h"

#include "common/config.h"        // libpatch_config::use_protocol_v1_6()
#include "../oem/blmjciaapa.h"    // AapConnectionManager_* accessors
#include "../oem/libdbus.h"
#include "common/thread_util.h"

#include <pthread.h>
#include <cstdlib>
#include <cstdio>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <string>

namespace {

// cb_list slot 3 = RaceAap::ProjectionStatusCb(void* user, int ev,
// void* data). Authoritative layout from RaceAap::Init (blmjciaapa.so
// FW 74.00.324A, file offset 0x8c64c) — a 76-byte / 19-word-slot table:
//   [0] OnErrorCb          [5] BtPairingReqCb    [10] NULL (nav/HUD slot)
//   [1] SessionStatusCb    [6] BtAuthStatusCb    [11..17] NULL
//   [2] StreamRequestCb    [7] NULL              [18] user_ctx
//   [3] ProjectionStatusCb [8] MediaPlaybackStatusCb
//   [4] AudioStateCb       [9] PhoneStatusCb
// Slot 18 is the user context the SDK passes as the 1st arg to every
// callback (OEM leaves it NULL). Re-harvest after any OEM update.
constexpr int kCbListProjSlot = 3;

// AAP_ProjectionEventType values (RaceAap Enum2Str table):
//   0x400 NONE, 0x401 ON, 0x402 OFF, 0x403 SETUP, 0x404 CONFIG.
constexpr int kAapProjectionSetup = 0x403;

// USB device name (AapConnectionManager + 0x54) presented by the
// wireless dongle in passthrough mode. A wired phone reports its own
// model name (e.g. "Pixel 9 Pro XL") instead.
constexpr char const *kDongleDevNames[] = {
    "AAWireless",
    "smartBox",
    "smartlinkBox",
    "carplay",
    "carlink",
    "motorolama1",
    "AutoKit",
    "Accessory",
    "CAR2"
};

// AapConnectionManager session-state values (`state_`, instance + 0xdc).
// Offset EMPIRICALLY pinned by a 3-state /proc/mem object diff
// (idle/wired/dongle, 2026-07-06): IDLE while idle, CONNECTED the moment
// the AOA session connects (the dongle parks here at GAL 1.6 waiting for
// the BT pairing request the phone never sends), ACTIVATED once
// ActivateAapSession runs.
constexpr int kStateIdle      = 0;   // IDLE
constexpr int kStateConnected = 2;   // CONNECTED (dongle parks here at GAL 1.6)
constexpr int kStateActivated = 3;   // ACTIVATED

constexpr const char *kServiceBusFallback =
    "unix:path=/tmp/dbus_service_socket";
constexpr const char *kNotificationPath = "/com/NNG/Api/Server";
constexpr const char *kNotificationIface =
    "com.NNG.Api.Server.NotificationBar";
constexpr const char *kNotificationMember = "ETCNotification";

// Original OEM ProjectionStatusCb, saved when we wrap slot 3. RaceAap::Init
// builds the cb_list once per BLM process, so a single global suffices.
typedef int (*ProjStatusFn)(void *user, int ev, void *data);
ProjStatusFn g_orig_proj_cb = nullptr;

// Single-flight guard for the deferred activator: at most one poll
// thread in flight at a time, re-armable per device connect once the
// previous one has exited.
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_activator_running = false;

// Match known dongle names as case-insensitive substrings of the current
// device name.
bool is_known_device(const char* currentDeviceName)
{
    if (currentDeviceName == nullptr) return false;

    const size_t elementCount = sizeof(kDongleDevNames) / sizeof(kDongleDevNames[0]);

    for (size_t i = 0; i < elementCount; ++i) {
        if (strcasestr(currentDeviceName, kDongleDevNames[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

void show_detected_device_notification(const char *device_name)
{
    if (device_name == nullptr || device_name[0] == '\0') {
        return;
    }

    const char *address = getenv("JCI_SERVICE_BUS");
    if (address == nullptr || address[0] == '\0') {
        address = kServiceBusFallback;
    }

    void *conn = dbus_connection_open_private(address, nullptr);
    if (conn == nullptr) {
        LOGW("bt16pair: could not open service bus for device notification");
        return;
    }
    dbus_connection_set_exit_on_disconnect(conn, 0);
    if (dbus_bus_register(conn, nullptr) == 0) {
        LOGW("bt16pair: could not register device-notification bus connection");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }

    void *msg = dbus_message_new_signal(
        kNotificationPath, kNotificationIface, kNotificationMember);
    if (msg == nullptr) {
        LOGW("bt16pair: could not allocate device-notification signal");
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }

    char text[160];
    snprintf(text, sizeof(text), "Detected device: '%.142s'", device_name);
    const char *text1 = text;
    const char *text2 = "";
    // Generated callback ABI is int32,string,string,int32 (not bool for beep).
    int32_t notification_type = 0;
    int32_t play_beep = 0;
    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    bool marshalled =
        dbus_message_iter_append_basic(
            &iter, DBUS_TYPE_INT32, &notification_type) != 0 &&
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &text1) != 0 &&
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &text2) != 0 &&
        dbus_message_iter_append_basic(
            &iter, DBUS_TYPE_INT32, &play_beep) != 0;

    if (!marshalled) {
        LOGW("bt16pair: could not marshal device-notification signal");
    } else if (dbus_connection_send(conn, msg, nullptr) == 0) {
        LOGW("bt16pair: could not send device-notification signal");
    } else {
        dbus_connection_flush(conn);
        LOGD("bt16pair: displayed detected device notification: \"%s\"", text);
    }

    dbus_message_unref(msg);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
}

// Deferred activator.
//
// The ProjectionStatusCb runs on the SDK IPC thread when projection is
// set up. On a wired phone the OEM then receives the in-band Bluetooth
// pairing request, calls NotifyBtPairingResult(success), and that runs
// ActivateAapSession — which is what actually gives AA the video focus.
// Over wireless dongles at GAL 1.6 the phone deliberately skips that
// pairing request, so the OEM parks at state CONNECTED(2) forever and
// video focus is never granted.
//
// This thread only ever runs for the dongle (the caller gates on
// dev=="AAWireless", "smartBox" etc), so we watch for that exact stuck state.
// Each poll:
//   - if AA already holds video focus, or state==ACTIVATED(3), a genuine
//     activation happened (GAL 1.5, or a pairing that did arrive) — do
//     nothing.
//   - if state==CONNECTED(2) — this is the dongle parked waiting for
//     the missing BT pairing — call ActivateAapSession(cm) directly,
//     once. That is exactly the activation the missing pairing-result
//     path performs (StartResourcesControl, ResumeLastMode,
//     NotifyBTConnectionComplete, state:=ACTIVATED(3)); it takes no
//     precondition.
//   - otherwise (state IDLE(0) / transient) keep polling until the AOA
//     session connects and state becomes CONNECTED(2), up to the grace
//     ceiling.
// The session-state offset (0xdc) was empirically triangulated, so gating
// on state==CONNECTED(2) is now safe. Single-flight guard + once-per-SETUP
// nature prevent any double activation.
void *activator_thread(void *)
{
    const int        kGraceTries = 30;     // ~3 s ceiling to reach CONNECTED(2)
    const useconds_t kStepUs     = 100000; // 100 ms poll interval

    LOGD("bt16pair: activator thread started (poll up to %d x %d ms for "
         "state==CONNECTED(2), then force ActivateAapSession)",
         kGraceTries, static_cast<int>(kStepUs / 1000));

    bool injected = false;
    for (int i = 0; i < kGraceTries; ++i) {
        void *aap = Singleton_AapProc_GetInstance();
        void *vm  = aap ? AapProc_GetVideoManager(aap) : nullptr;
        int   focus = vm ? VideoManager_IsAAVideoInFocus(vm) : 0;
        void *cm  = AapConnectionManager_instance();
        int   state = cm ? AapConnectionManager_session_state(cm) : -1;

        LOGV("bt16pair: activator poll #%d/%d: aap=%p vm=%p focus=%d cm=%p "
             "state=%d", i + 1, kGraceTries, aap, vm, focus, cm, state);

        // Natural activation already happened — nothing to do.
        if (focus || state == kStateActivated) {
            LOGD("bt16pair: AA activated on its own after ~%d ms "
                 "(focus=%d state=%d); no inject needed", i * 100, focus, state);
            break;
        }

        // The dongle-stuck state: AOA session up, parked waiting for a BT
        // pairing that will never arrive. Force the activation the OEM
        // would have run on pairing success.
        if (cm && state == kStateConnected) {
            LOGD("bt16pair: state==CONNECTED(2) (dongle stuck) after "
                 "~%d ms — forcing ActivateAapSession(cm=%p)", i * 100, cm);
            AapConnectionManager_ActivateAapSession(cm);
            injected = true;

            void *vm2 = aap ? AapProc_GetVideoManager(aap) : nullptr;
            int   f2  = vm2 ? VideoManager_IsAAVideoInFocus(vm2) : 0;
            int   s2  = AapConnectionManager_session_state(cm);
            LOGD("bt16pair: post-inject AA video focus=%d state=%d", f2, s2);
            break;
        }

        usleep(kStepUs);
    }

    if (!injected) {
        // Grace elapsed without ever seeing the CONNECTED(2) state (or a
        // natural activation). The AOA session never reached CONNECTED(2),
        // so there is nothing to activate — do NOT blind-inject.
        void *cm    = AapConnectionManager_instance();
        int   state = cm ? AapConnectionManager_session_state(cm) : -1;
        LOGD("bt16pair: grace window elapsed without state==CONNECTED(2) "
             "(final state=%d); not injecting", state);
    }

    pthread_mutex_lock(&g_mu);
    g_activator_running = false;
    pthread_mutex_unlock(&g_mu);
    LOGD("bt16pair: activator thread exiting (injected=%d)", injected ? 1 : 0);
    return nullptr;
}

void spawn_activator_once(void)
{
    pthread_mutex_lock(&g_mu);
    bool spawn = !g_activator_running;
    if (spawn) g_activator_running = true;
    pthread_mutex_unlock(&g_mu);

    if (!spawn) {
        LOGD("bt16pair: activator already in flight; not re-spawning");
        return;
    }

    pthread_t t;
    if (preload_thread_create(&t, activator_thread, nullptr) == 0) {
        pthread_detach(t);
        LOGD("bt16pair: spawned deferred pairing-bypass activator");
    } else {
        LOGW("bt16pair: pthread_create failed; clearing single-flight guard");
        pthread_mutex_lock(&g_mu);
        g_activator_running = false;
        pthread_mutex_unlock(&g_mu);
    }
}

// Transparent ProjectionStatusCb wrapper. Always chains to the OEM
// callback first (keeps native behaviour intact), then — only for the
// wireless GAL-1.6 case — arms the deferred activator on SETUP.
int our_projection_status_cb(void *user, int ev, void *data)
{
    int rc = g_orig_proj_cb ? g_orig_proj_cb(user, ev, data) : 0;

    // With wireless=true, aaw_session owns session activation. This bypass is
    // for USB wireless-AA dongles; stand down to avoid a second activation.
    if (libpatch_config::wireless_enabled()) {
        return rc;
    }

    // Per-invocation trace (verbose; every projection event reaches here).
    // Resolve the connection manager / device name / session state for both
    // the log and the SETUP gate below.
    void       *cm    = AapConnectionManager_instance();
    const char *dev   = cm ? AapConnectionManager_dev_name(cm) : nullptr;
    int         state = cm ? AapConnectionManager_session_state(cm) : -1;
    bool        all_devices = libpatch_config::bt_pairing_bypass_all_devices();
    LOGV("bt16pair: proj cb: ev=0x%x user=%p data=%p v1_6=%d cm=%p "
            "dev=\"%s\" state=%d all_devices=%d (orig_cb=%p rc=%d)",
         ev, user, data, libpatch_config::use_protocol_v1_6() ? 1 : 0, cm,
            dev ? dev : "(null)", state, all_devices ? 1 : 0,
         reinterpret_cast<void *>(g_orig_proj_cb), rc);

    if (ev != kAapProjectionSetup) {
        LOGV("bt16pair: proj cb: ev 0x%x is not AAP_PROJECTION_SETUP (0x%x); "
             "not arming", ev, kAapProjectionSetup);
        return rc;
    }
    if (!libpatch_config::use_protocol_v1_6()) {
        LOGD("bt16pair: proj cb: SETUP but use_protocol_v1_6 disabled; not arming");
        return rc;
    }
    if (!cm) {
        LOGW("bt16pair: proj cb: SETUP but AapConnectionManager unavailable; "
             "not arming");
        return rc;
    }

    if (libpatch_config::bt_pairing_show_device_notification()) {
        show_detected_device_notification(dev);
    }

    if (!all_devices && (!dev || !is_known_device(dev))) {
        LOGD("bt16pair: proj cb: SETUP but dev=\"%s\" is not known dongle; not arming",
             dev ? dev : "(null)");
        return rc;
    }

    LOGD("bt16pair: AAP_PROJECTION_SETUP on \"%s\" with use_protocol_v1_6 "
        "(state=%d all_devices=%d) — arming BT-pairing bypass",
        dev ? dev : "(null)", state, all_devices ? 1 : 0);

    spawn_activator_once();
    return rc;
}

} // namespace

void bt16pair_pre_aap_create_session(void *cb_list)
{
    if (!cb_list) {
        LOGW("bt16pair_pre_aap_create_session: NULL cb_list, skipping");
        return;
    }

    void **slots = static_cast<void **>(cb_list);

    // Idempotent across aap_create_session retries (the OEM re-passes the
    // same cb_list stack buffer on rc==0x104): never re-save our own
    // wrapper as the "original", or we'd recurse infinitely.
    if (slots[kCbListProjSlot] ==
        reinterpret_cast<void *>(&our_projection_status_cb)) {
        return;
    }

    g_orig_proj_cb =
        reinterpret_cast<ProjStatusFn>(slots[kCbListProjSlot]);
    slots[kCbListProjSlot] =
        reinterpret_cast<void *>(&our_projection_status_cb);

    LOGD("bt16pair_pre_aap_create_session: cb_list=%p slot[%d] %p -> %p "
         "(proj status wrapper)",
         cb_list, kCbListProjSlot, reinterpret_cast<void *>(g_orig_proj_cb),
         reinterpret_cast<void *>(&our_projection_status_cb));
}
