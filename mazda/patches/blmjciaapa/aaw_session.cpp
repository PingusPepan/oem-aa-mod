// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libpatch-blmjciaapa.so — wireless Android Auto session start/stop.
//
// The head unit starts an Android Auto session only when the USB device manager
// announces a device:
//
//   DevMgr -> AapConnectionManager::DevMgrEnabledCapCb(ctx, devInfo, x)
//     -> worker: ReceiveDevMgrEnabledCap(devInfo)
//          -> StoreDeviceInfo(devInfo)                 // name, serial, type, vid
//          -> StartSession() if devInfo.type == 0xb01f
//     -> RaceAap::StartSession -> aap_start_session -> MMUI shows Android Auto
//
// A phone on Wi-Fi is never a USB device, so none of this runs. Once the phone
// has joined the AP, aaw_handler emits com.jci.aaw.WirelessAaConnected; this
// listener replays the device-connect event for it, and the aap_start_session
// hook (start_session.cpp) switches the session's transport from USB to Wi-Fi.
// WirelessAaDisconnected (from aaw_listend) replays the device-disconnect.
//
// Identity: the phone sends its serial in the Bluetooth handshake (version
// response, field 3). It is the same serial the wired path reports, so the
// unit's pairing record matches. The device name is set to the serial; the
// phone's make and model arrive later over the AA protocol.
//
// Thread safety: DevMgrEnabledCapCb / DevMgrDisabledCapCb only copy the device
// info into a worker and post it. The work runs on the OEM's own worker thread,
// exactly as for a real USB event, so calling them from this thread is safe.
//
// jciAAPA is configured with reset_board="yes": a crash here reboots the head
// unit. OEM objects are therefore re-resolved on every use, never held across
// a sleep, and never passed on as a null `this`.

#define LOG_TAG "AAWSESS"
#include "log.h"

#include "common/config.h"
#include "common/thread_util.h"
#include "oem/blmjciaapa.h"
#include "oem/libdbus.h"
#include "aaw_session.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace {

// ---- sDevMgrDeviceInfo ---------------------------------------------------
// The worker copies exactly kDevInfoSize bytes, so the buffer must be that big.
constexpr uint32_t kAaDeviceType = 0xb01fu;   // Android Auto device type
constexpr unsigned kDevInfoSize  = 0x84c;
constexpr unsigned kOffType      = 0x000;     // u32
constexpr unsigned kOffHandle    = 0x004;     // u32, DevMgr handle
constexpr unsigned kOffVid       = 0x014;     // u32, low 16 bits = USB vendor id
constexpr unsigned kOffName      = 0x01c;     // char[]
constexpr unsigned kOffSerial    = 0x328;     // char[]
// The OEM copies name and serial into adjacent 0x40-byte slots with no length
// cap, so both are limited to 63 characters here.
constexpr unsigned kStrFieldSz   = 0x40;

// ---- D-Bus ---------------------------------------------------------------
constexpr const char *kBusAddress = "unix:path=/tmp/dbus_service_socket";
constexpr const char *kIface      = "com.jci.aaw";
constexpr const char *kMemberConn = "WirelessAaConnected";     // from aaw_handler
constexpr const char *kMemberDisc = "WirelessAaDisconnected";  // from aaw_listend
constexpr const char *kMatchRule  = "type='signal',interface='com.jci.aaw'";

// The phone can report success more than once per join. Connect signals within
// this window are ignored. A disconnect clears the window, so a quick
// reconnect is never dropped.
constexpr long kDebounceSec = 5;
long g_last_connect = 0;

// True while the wireless session is the live one. A wired start while it is
// set is a wireless->wired handoff.
volatile bool g_wireless_active = false;

// Bumped on every connect and disconnect. Each watcher thread captures it at
// spawn and stops as soon as it changes, so a watcher can never act on the
// next session.
volatile int g_session_gen = 0;

// BCA's device id of the phone behind the current wireless session, from the
// connect signal. Set before the session's watcher is spawned.
volatile uint32_t g_session_bt_dev = 0;

// Is a phone in Android Open Accessory mode (wired Android Auto)? Any brand
// re-enumerates as 18d1:2d00..2d05 in that mode. While one is present the OEM
// owns the wired session and wireless replays stand down. Unreadable /sys
// counts as "no", so wireless is never blocked on a guess.
bool usb_aoa_present()
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        char path[300], buf[16];
        snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n < 4) continue;
        buf[n] = '\0';
        if (strncmp(buf, "18d1", 4) != 0) continue;
        snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n < 4) continue;
        buf[n] = '\0';
        if (strncmp(buf, "2d0", 3) == 0 && buf[3] >= '0' && buf[3] <= '5')
            found = true;
    }
    closedir(d);
    return found;
}

// ---- OEM session state ---------------------------------------------------
// RaceAap+0x08 holds the AAP SDK's current session status (the BLM's
// eCurStatus). RaceAap+0x04 is the session handle.
constexpr unsigned kRaceAapStatusOff = 0x08;

constexpr uint32_t kSessInvalid      = 0x000u;   // fully released; a new start succeeds
constexpr uint32_t kSessDisconnected = 0x300u;   // phone hung up, SDK not released yet

// AapConnectionManager state machine (see AapConnectionManager_session_state).
constexpr int kAcmStateIdle      = 0;
constexpr int kAcmStateConnected = 2;
constexpr int kAcmStateActivated = 3;
constexpr int kAcmStateDisconn   = 4;

// SDK session status, or 0xffffffff when RaceAap is not resolvable. Unknown is
// never treated as INVALID.
uint32_t read_session_state()
{
    void *inst = Singleton_AapProc_GetInstance();
    void *race = inst ? AapProc_GetRaceAap(inst) : nullptr;
    if (!race) return 0xffffffffu;
    return *(volatile uint32_t *)((char *)race + kRaceAapStatusOff);
}

// ---- Bluetooth handshake -------------------------------------------------
//
// Once the session is CONNECTED the OEM calls Bluetooth::StartBTConnection: it
// sets Bluetooth+0x15 = 1 and asks BCA to start Android Auto pairing. BCA then
// waits for the phone's AA BluetoothPairingRequest. When it arrives (SDK ->
// RaceAap::BtPairingReqCb -> Bluetooth::RequestBTPairing), BCA binds the phone
// to Android Auto: voice stays with AA, the phone's A2DP link to the head
// unit's media player is dropped, and calls are no longer handled as native
// phone calls. BCA then answers ReadyToPair + PairingResult(1), which runs
// ActivateAapSession. Over USB the phone sends that request within 1-2 s.
//
// Over Wi-Fi the phone sends its first pairing request only about two minutes
// into the session, and repeats it every two minutes. Until then the session
// is not activated, media audio plays through the head unit's Bluetooth
// player, and a call placed from AA switches the screen to the OEM phone app.
//
// So when StartBTConnection fires, the watcher delivers the pairing request
// itself through the same SDK callback the phone's request uses. The OEM then
// runs its normal flow within seconds. The phone's own later request takes the
// same path and changes nothing.
//
// The request must name the phone's Bluetooth address. aaw_listend learns BCA's
// device id of the phone that opened the AAW channel and aaw_handler relays it
// with the connect signal; the watcher looks that id up among the
// device-manager ids that are index-aligned with the Bluetooth object's
// paired-address list, so any number of paired phones works. If the id is not
// in the list the request is skipped, since naming the wrong phone would bind
// the wrong one.
//
// Fallback, also used when BCA does not answer: if the session is not
// ACTIVATED within kBcaActivateCapMs, send RaceAap::BtReadyToPair(1) and
// deliver PairingResult(1) through the OEM's own handler. That activates the
// session without BCA's binding.
//
// None of this issues a system Bluetooth command: these are AA protocol
// messages and an OEM D-Bus handler, and every OEM entry point used only posts
// a worker. Pairing, HFP and A2DP stay with bds/BCA.

// Bluetooth object fields.
constexpr unsigned kBtFlag0c     = 0x0c;  // int, logged only
constexpr unsigned kBtInProgress = 0x15;  // u8, 1 once StartBTConnection ran
constexpr unsigned kBtPairedMacs = 0x1c;  // u8[6] per entry, byte-reversed
constexpr unsigned kBtConnMac    = 0x46;  // u8[6], connected device (calls only)
constexpr unsigned kBtPairedCnt  = 0x4c;  // int, entries in the paired list
constexpr int      kMaxPairedDevs = 7;    // capacity of the paired list

// Over USB the request -> bind -> activate flow takes 1-2 s.
constexpr int kBcaActivateCapMs  = 6000;
constexpr int kBcaActivateStepMs = 100;
// Pairing method the phone itself requests (AAP_BT_PAIRING_NUMERIC).
constexpr int kPhonePairingMethod = 5;
// Wait for the phone's HFP profile and the video sink before a direct
// activation that was not preceded by the pairing request.
constexpr int kDirectActivateSettleMs = 2000;

void bt_dump(const unsigned char *b, const char *when)
{
    int cnt = *(const int *)(b + kBtPairedCnt);
    const unsigned char *cm = b + kBtConnMac;
    LOGE("bt-watch[%s]: flag0c=%d inProgress=%u paired=%d connMac=%02x:%02x:%02x:%02x:%02x:%02x",
         when, *(const int *)(b + kBtFlag0c), (unsigned)*(b + kBtInProgress), cnt,
         cm[0], cm[1], cm[2], cm[3], cm[4], cm[5]);
    for (int i = 0; i < cnt && i < 8; ++i) {
        const unsigned char *m = b + kBtPairedMacs + i * 6;
        LOGE("bt-watch:   paired[%d]=%02x:%02x:%02x:%02x:%02x:%02x",
             i, m[0], m[1], m[2], m[3], m[4], m[5]);
    }
}

// Build the phone's pairing request from the paired-device list. The entry is
// picked by bt_dev, BCA's device id of the phone that opened this session's AAW
// channel (relayed by aaw_handler), matched against the device-manager ids that
// are index-aligned with the list. Entries are stored byte-reversed relative to
// the text form ("48:EF:1C:D3:D8:DD" is stored as dd d8 d3 1c ef 48). Returns
// false if the id is not in the list.
bool phone_pair_request_from_paired_list(const unsigned char *b, uint32_t bt_dev,
                                         AAP_BtPairReqEvent *ev)
{
    const int cnt = *(const int *)(b + kBtPairedCnt);
    if (cnt < 1 || cnt > kMaxPairedDevs) {
        LOGE("bt-watch: paired count %d out of range; activating directly instead", cnt);
        return false;
    }
    int idx = -1;
    const uint32_t *ids = Bluetooth_paired_dev_handles();
    for (int i = 0; ids && i < cnt; ++i)
        if (ids[i] == bt_dev) { idx = i; break; }
    if (idx < 0) {
        LOGE("bt-watch: BT device id %u is not in the paired list (%d entries); "
             "activating directly instead", (unsigned)bt_dev, cnt);
        return false;
    }
    const unsigned char *m = b + kBtPairedMacs + idx * 6;
    if (!(m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) {
        LOGE("bt-watch: paired[%d] is all-zero; activating directly instead", idx);
        return false;
    }
    LOGE("bt-watch: AA phone = paired[%d] (BT device id %u)", idx, (unsigned)bt_dev);
    memset(ev, 0, sizeof *ev);
    snprintf(ev->mac, sizeof ev->mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[5], m[4], m[3], m[2], m[1], m[0]);
    ev->method = kPhonePairingMethod;
    return true;
}

enum class WaitResult { Activated, Gone, TimedOut };

// Wait for the OEM to activate the session after the pairing request.
// Gone = the session ended or was replaced meanwhile.
WaitResult wait_for_activation(int my_gen)
{
    int st = -1;
    for (int waited = 0; waited < kBcaActivateCapMs; waited += kBcaActivateStepMs) {
        usleep(kBcaActivateStepMs * 1000);
        if (g_session_gen != my_gen) {
            LOGE("bt-watch: session changed while waiting for activation");
            return WaitResult::Gone;
        }
        void *cm = AapConnectionManager_instance();
        st = cm ? AapConnectionManager_session_state(cm) : -1;
        if (st == kAcmStateActivated) {
            LOGE("bt-watch: OEM activated the session %dms after the pairing request",
                 waited + kBcaActivateStepMs);
            return WaitResult::Activated;
        }
        if (st == kAcmStateIdle || st == kAcmStateDisconn) {
            LOGE("bt-watch: session left CONNECTED (state=%d) while waiting", st);
            return WaitResult::Gone;
        }
    }
    LOGE("bt-watch: not activated within %dms (state=%d); activating directly",
         kBcaActivateCapMs, st);
    return WaitResult::TimedOut;
}

// One-shot per wireless session; gives up after ~30 s.
void *bt_ready_watch(void *arg)
{
    const int my_gen = (int)(intptr_t)arg;

    {
        void *inst0 = Singleton_AapProc_GetInstance();
        void *bt0   = inst0 ? AapProc_GetBluetooth(inst0) : nullptr;
        if (!bt0) { LOGE("bt-watch: Bluetooth object unresolved"); return nullptr; }
        bt_dump((const unsigned char *)bt0, "start");
    }

    for (int i = 0; i < 150; ++i) {
        if (g_session_gen != my_gen) {
            LOGE("bt-watch: session changed (gen %d -> %d)", my_gen, g_session_gen);
            return nullptr;
        }
        void *inst = Singleton_AapProc_GetInstance();
        void *bt   = inst ? AapProc_GetBluetooth(inst) : nullptr;
        if (!bt) { LOGE("bt-watch: Bluetooth object gone"); return nullptr; }
        const unsigned char *b = (const unsigned char *)bt;

        if (*(volatile const unsigned char *)(b + kBtInProgress) == 1) {
            LOGE("bt-watch: StartBTConnection fired");
            bt_dump(b, "pairing");

            // Preferred: deliver the phone's pairing request and let the OEM
            // bind and activate.
            AAP_BtPairReqEvent ev;
            bool injected = false;
            if (phone_pair_request_from_paired_list(b, g_session_bt_dev, &ev)) {
                LOGE("bt-watch: -> RaceAap::BtPairingReqCb(mac=%s, method=%d)",
                     ev.mac, ev.method);
                RaceAap_BtPairingReqCb(nullptr, &ev);   // copies ev, posts a worker
                injected = true;
                switch (wait_for_activation(my_gen)) {
                case WaitResult::Activated: return nullptr;
                case WaitResult::Gone:      return nullptr;
                case WaitResult::TimedOut:  break;
                }
            }

            // Fallback: activate directly.
            void *inst2 = Singleton_AapProc_GetInstance();
            void *ra    = inst2 ? AapProc_GetRaceAap(inst2) : nullptr;
            if (!ra) {
                LOGE("bt-watch: RaceAap unresolved; not activating");
                return nullptr;
            }
            LOGE("bt-watch: -> RaceAap::BtReadyToPair(1)");
            RaceAap_BtReadyToPair(ra, 1);

            // Posts ReceivePairingResult -> NotifyBtPairingResult(1) ->
            // ActivateAapSession on the OEM worker. reply=NULL: no D-Bus reply.
            // After a pairing request the wait above already covered the settle.
            if (!injected) usleep(kDirectActivateSettleMs * 1000);
            if (g_session_gen != my_gen) {
                LOGE("bt-watch: session changed; not activating");
                return nullptr;
            }
            LOGE("bt-watch: -> AAPA_BTPairingResult_svc(1)");
            AAPA_BTPairingResult_svc(nullptr, nullptr, 1);
            return nullptr;
        }
        usleep(200000);
    }
    LOGE("bt-watch: StartBTConnection did not fire within 30s");
    return nullptr;
}

void snapshot_wired_dm_handle(const char *when);
void reset_handoff_state(const char *why);

// ---- wireless connect / disconnect ---------------------------------------

void replay_device_connect(const char *serial, uint32_t bt_dev)
{
    if (usb_aoa_present()) {
        // Replaying now would put a second, Wi-Fi session over the wired one.
        LOGE("wireless connect ignored: a phone is in wired AA mode (18d1:2d0x)");
        return;
    }
    long now = (long)time(nullptr);
    if (g_last_connect != 0 && now - g_last_connect < kDebounceSec) {
        LOGD("wireless connect within %lds of the last one; ignored", now - g_last_connect);
        return;
    }
    g_last_connect = now;
    // Wireless coming back means the cable is out; any handoff state is stale.
    reset_handoff_state("wireless session returning");
    g_wireless_active = true;
    g_session_bt_dev = bt_dev;
    const int gen = ++g_session_gen;

    if (!serial || !serial[0]) serial = "AndroidAuto";

    unsigned char info[kDevInfoSize];
    memset(info, 0, sizeof info);
    uint32_t type   = kAaDeviceType;
    uint16_t vid    = aaw_session::kMarkerVid;
    uint32_t handle = aaw_session::kMarkerHandle;
    memcpy(info + kOffType,   &type,   sizeof type);
    memcpy(info + kOffHandle, &handle, sizeof handle);
    memcpy(info + kOffVid,    &vid,    sizeof vid);
    snprintf((char *)(info + kOffName),   kStrFieldSz, "%s", serial);
    snprintf((char *)(info + kOffSerial), kStrFieldSz, "%s", serial);

    LOGE("wireless AA up: replaying device connect (serial=\"%s\", BT device id %u)",
         serial, (unsigned)bt_dev);
    int rc = AapConnectionManager_DevMgrEnabledCapCb(nullptr, info, nullptr);
    LOGE("DevMgrEnabledCapCb -> %d (StartSession runs on the OEM worker)", rc);

    pthread_t bw;
    if (preload_thread_create(&bw, bt_ready_watch, (void *)(intptr_t)gen) == 0)
        pthread_detach(bw);
    else
        LOGE("could not spawn bt-watch; the session may not activate");
}

// A phone that walks away sends no FIN and there is no USB unplug, so without
// this aap_service would stay blocked on the dead TCP socket for ~40 s, the
// screen would stay black, and a reconnect inside that window would land on the
// stale session. The replayed disconnect runs StopSession -> aap_stop_session
// and DeactivateAapSession on the same OEM worker as the connect, so a
// disconnect followed by a reconnect is always torn down before it is set up.
void replay_device_disconnect()
{
    if (usb_aoa_present()) {
        // The Wi-Fi link dropped because the phone moved to the cable: a
        // wireless->wired handoff has begun. Do not stop the session; the wired
        // one is about to use it.
        LOGE("wireless disconnect ignored: a phone is in wired AA mode (handoff)");

        // The OEM arms its "Device Not Responding" watchdog at cable-in and it
        // fires ~2.3 s later, before the wired session can come up (its first
        // start is refused until the wireless session releases). That is a
        // false alarm. Stopping the timer later is too late: RaceAap::StartSession
        // spends ~2 s on a date/time exchange before calling aap_start_session,
        // and stopping the timer does not cancel an expiry already queued. This
        // point is reached ~0.9 s after cable-in. The OEM re-arms the watchdog on
        // the next device event, so a genuinely dead phone is still reported.
        snapshot_wired_dm_handle("cable-in");
        void *cm = AapConnectionManager_instance();
        if (cm) {
            AapConnectionManager_StopDeviceNotRespondingTimer(cm);
            LOGE("handoff: stopped the Device-Not-Responding watchdog");
        } else {
            LOGE("handoff: AapConnectionManager unresolved; cannot stop the "
                 "Device-Not-Responding watchdog");
        }
        return;
    }
    ++g_session_gen;
    g_last_connect = 0;
    g_wireless_active = false;

    // Only the type is checked, but the worker copies the full struct.
    unsigned char info[kDevInfoSize];
    memset(info, 0, sizeof info);
    uint32_t type = kAaDeviceType;
    memcpy(info + kOffType, &type, sizeof type);

    // sDevMgrDetachCause: two dwords are read (logged only). Must be non-NULL.
    unsigned char cause[16];
    memset(cause, 0, sizeof cause);

    LOGE("wireless AA down: replaying device disconnect");
    AapConnectionManager_DevMgrDisabledCapCb(nullptr, info, cause, nullptr);
}

// Private connection to the service bus. Retries while the bus daemon starts.
void *open_service_bus()
{
    for (int attempt = 0; attempt < 60; ++attempt) {
        void *conn = dbus_connection_open_private(kBusAddress, nullptr);
        if (conn) {
            if (dbus_bus_register(conn, nullptr)) {
                dbus_connection_set_exit_on_disconnect(conn, 0);
                return conn;
            }
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
        }
        sleep(2);
    }
    return nullptr;
}

void *listener_main(void *)
{
    void *conn = open_service_bus();
    if (!conn) {
        LOGE("cannot open the service bus; wireless AA sessions will not start");
        return nullptr;
    }
    dbus_bus_add_match(conn, kMatchRule, nullptr);
    dbus_connection_flush(conn);
    LOGE("listening for %s.{%s,%s}", kIface, kMemberConn, kMemberDisc);

    for (;;) {
        dbus_connection_read_write(conn, 300);
        void *msg;
        while ((msg = dbus_connection_pop_message(conn)) != nullptr) {
            if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
                const char *m = dbus_message_get_member(msg);
                if (m && strcmp(m, kMemberConn) == 0) {
                    // Arguments: the phone's serial, then BCA's device id of
                    // the phone.
                    char serial[kStrFieldSz] = {0};
                    uint32_t bt_dev = 0;
                    DBusMessageIter it;
                    if (dbus_message_iter_init(msg, &it) &&
                        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                        const char *s = nullptr;
                        dbus_message_iter_get_basic(&it, &s);
                        if (s) snprintf(serial, sizeof serial, "%s", s);
                        if (dbus_message_iter_next(&it) &&
                            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32)
                            dbus_message_iter_get_basic(&it, &bt_dev);
                    }
                    replay_device_connect(serial, bt_dev);
                } else if (m && strcmp(m, kMemberDisc) == 0) {
                    replay_device_disconnect();
                }
            }
            dbus_message_unref(msg);
        }
    }
    return nullptr;
}

// ---- wireless -> wired handoff -------------------------------------------
//
// On cable-in the phone does not ask for an in-place transport switch (no
// UserSwitchRequest). It closes its Wi-Fi TCP socket and expects the head unit
// to start a new session over USB. The OEM issues its wired aap_start_session
// ~0.4 s after the phone enumerates, while the wireless session still occupies
// the SDK, so the start is refused with 279. The old session reaches INVALID
// ~3-3.5 s later and the OEM never tries again, although the cable stays
// usable for tens of seconds.
//
// The fix lets the OEM's start run and fail, then re-issues it from a side
// thread once the session is INVALID. A wired start issued before INVALID is
// always refused; one issued after it always succeeds. RaceAap::StartSession
// only logs and returns the result, so a refused start leaves no state behind.
//
// Approaches that do not work:
// - Holding the wired start until INVALID: the phone reopens its Wi-Fi socket
//   ~0.5 s in, which restarts the SDK's release, so INVALID never arrives
//   while the start is held.
// - Swallowing the wired start: the phone has already left Wi-Fi, so the
//   wired session is its only way back. It then retries Wi-Fi every ~11 s.
// - Tearing the wireless session down first (StopSession, ByeBye): the phone
//   reconnects over the still-running AP and collides with the wired session.

constexpr int kStartRefused = 279;   // another session is still active

// INVALID arrives ~3-3.5 s after the refusal; the cable stays connected for
// 19-57 s. 15 s covers the release with margin.
constexpr int kRetryCapMs  = 15000;
constexpr int kRetryStepMs = 50;

constexpr int kActivateCapMs  = 15000;
constexpr int kActivateStepMs = 50;
// Without this the activation can land ~200 ms before the video sink is set up,
// which leaves the screen black.
constexpr int kVideoSinkSettleMs = 2000;

volatile bool g_handoff_start_pending = false;

// Last real DevMgr handle the OEM queried device settings for.
volatile unsigned g_real_dm_handle = 0;
volatile bool     g_real_dm_valid  = false;

// The wired phone's identity, from the StartInfo the OEM built for it.
volatile uint16_t g_handoff_vid = 0;
char              g_handoff_serial[64] = {0};

// The DevMgr handle the OEM stored for the current device. Deactivating the
// wireless session clears it within ~300 ms, so it is captured as soon as a
// handoff is detected.
volatile unsigned g_wired_dm_snap = 0;

// Clear all per-handoff state. Called whenever a handoff ends, however it ends:
// a DevMgr handle belongs to one attachment, and reusing it for a later cable
// or another phone would make the settings query fail.
void reset_handoff_state(const char *why)
{
    const bool had = g_wired_dm_snap || g_real_dm_valid || g_handoff_serial[0];
    g_wired_dm_snap  = 0;
    g_real_dm_handle = 0;
    g_real_dm_valid  = false;
    g_handoff_vid    = 0;
    g_handoff_serial[0] = '\0';
    if (had)
        LOGE("handoff: state cleared (%s)", why);
}

void snapshot_wired_dm_handle(const char *when)
{
    // First value of a handoff wins: by later calls the OEM has already zeroed
    // the field.
    if (g_wired_dm_snap) return;
    void *cm = AapConnectionManager_instance();
    unsigned h = cm ? AapConnectionManager_dev_handle(cm) : 0u;
    if (!h || h == aaw_session::kMarkerHandle) return;
    g_wired_dm_snap = h;
    LOGE("handoff: saved DevMgr handle 0x%x at %s", h, when);
}

struct WiredRetry {
    int      gen;
    uint16_t vid;
    char     serial[64];
};

// Activate the re-issued wired session. The replayed device-connect usually
// makes the OEM activate on its own; this is the safety net for when it does
// not, which otherwise leaves a live session behind a black screen. A late call
// is harmless: NotifyBtPairingResult acts only while the state is CONNECTED.
//
// It does not wait for StartBTConnection the way bt_ready_watch does: on a
// handoff the phone's Bluetooth never dropped, and BCA's pairing attempt at
// cable-in times out before the session connects.
void *wired_activate_watch(void *arg)
{
    const int my_gen = (int)(intptr_t)arg;
    int waited = 0;
    int st = -1;
    // Gate on the AapConnectionManager state, which is what the OEM's own
    // pairing-result guard checks. The SDK status reports CONNECTED ~2.3 s
    // before the USB transport is configured, which is too early.
    while (waited < kActivateCapMs) {
        void *cm = AapConnectionManager_instance();
        st = cm ? AapConnectionManager_session_state(cm) : -1;
        if (st == kAcmStateConnected) break;
        usleep(kActivateStepMs * 1000);
        waited += kActivateStepMs;
        if (g_session_gen != my_gen) {
            LOGE("wired activate: session changed after %dms", waited);
            return nullptr;
        }
        if (!usb_aoa_present()) {
            LOGE("wired activate: cable removed after %dms", waited);
            return nullptr;
        }
    }
    if (st != kAcmStateConnected) {
        LOGE("wired activate: not CONNECTED within %dms (state=%d)", waited, st);
        return nullptr;
    }

    usleep(kVideoSinkSettleMs * 1000);
    if (g_session_gen != my_gen) {
        LOGE("wired activate: session changed during the video-sink settle");
        return nullptr;
    }
    if (!usb_aoa_present()) {
        LOGE("wired activate: cable removed during the video-sink settle");
        return nullptr;
    }

    void *inst = Singleton_AapProc_GetInstance();
    void *ra   = inst ? AapProc_GetRaceAap(inst) : nullptr;
    if (ra) {
        LOGE("wired activate: -> RaceAap::BtReadyToPair(1)");
        RaceAap_BtReadyToPair(ra, 1);
    } else {
        LOGE("wired activate: RaceAap unresolved; skipping BtReadyToPair");
    }

    if (g_session_gen != my_gen) {
        LOGE("wired activate: session changed; not activating");
        return nullptr;
    }
    LOGE("wired activate: CONNECTED after %dms -> AAPA_BTPairingResult_svc(1)", waited);
    AAPA_BTPairingResult_svc(nullptr, nullptr, 1);
    return nullptr;
}

void *wired_retry_main(void *arg)
{
    WiredRetry *r = static_cast<WiredRetry *>(arg);
    int waited = 0;
    int dnr_tick = 0;
    bool nudged = false;
    uint32_t st = read_session_state();
    while (st != kSessInvalid && waited < kRetryCapMs) {
        usleep(kRetryStepMs * 1000);
        waited += kRetryStepMs;

        // The OEM re-arms the Device-Not-Responding watchdog after it is
        // stopped, so keep stopping it (every ~500 ms) until the session is free.
        if ((dnr_tick += kRetryStepMs) >= 500) {
            dnr_tick = 0;
            void *cmw = AapConnectionManager_instance();
            if (cmw) AapConnectionManager_StopDeviceNotRespondingTimer(cmw);
        }

        // After the phone hangs up, the SDK can sit at DISCONNECTED anywhere
        // from 350 ms to 32 s before releasing, with nothing in flight. Ask it to
        // release once. Only from DISCONNECTED, where no live session is left to
        // damage; StopSession takes no lock and does not deactivate the screen.
        if (!nudged && st == kSessDisconnected) {
            void *instn = Singleton_AapProc_GetInstance();
            void *ran   = instn ? AapProc_GetRaceAap(instn) : nullptr;
            if (ran) {
                nudged = true;
                LOGE("wired retry: session DISCONNECTED after %dms; "
                     "RaceAap::StopSession to release it", waited);
                RaceAap_StopSession(ran);
            }
        }
        if (r->gen != g_session_gen) {
            LOGE("wired retry: abandoned after %dms, a new session started", waited);
            reset_handoff_state("new session started");
            free(r);
            return nullptr;
        }
        st = read_session_state();
    }

    if (st != kSessInvalid) {
        LOGE("wired retry: session still 0x%x after %dms; not retrying",
             (unsigned)st, waited);
        reset_handoff_state("session never released");
        free(r);
        return nullptr;
    }
    if (!usb_aoa_present()) {
        LOGE("wired retry: session released after %dms but the cable is gone", waited);
        reset_handoff_state("cable removed");
        free(r);
        return nullptr;
    }

    g_wireless_active = false;

    // Re-enter through the OEM's device-connect path, not a raw
    // aap_start_session. The raw call skips the OEM's bookkeeping: no video sink
    // (black screen), and it can fail with 257. The device-connect path runs
    // start, video sink, transport and CONNECTED in order, in ~2.4 s.
    //
    // Every field comes from the live device: the handle from the OEM, the vid
    // and serial from the StartInfo the OEM built for this phone, and the name
    // from the OEM's stored USB device name (falling back to the serial).
    void       *cm   = AapConnectionManager_instance();
    const char *name = cm ? AapConnectionManager_dev_name(cm) : nullptr;
    if (!name || !name[0]) name = r->serial;

    // Handle sources, best first: captured from the OEM's settings query; the
    // handle the OEM currently stores; the copy saved at cable-in. On the first
    // handoff after boot the OEM never gets as far as the settings query, so
    // only the stored or saved handle exists.
    unsigned handle = 0;
    const char *hsrc = "none";
    if (g_real_dm_valid) { handle = g_real_dm_handle; hsrc = "settings query"; }
    if (!handle || handle == aaw_session::kMarkerHandle) {
        unsigned stored = cm ? AapConnectionManager_dev_handle(cm) : 0u;
        if (stored && stored != aaw_session::kMarkerHandle) { handle = stored; hsrc = "stored"; }
        else if (g_wired_dm_snap) { handle = g_wired_dm_snap; hsrc = "saved at cable-in"; }
    }
    if (!handle || handle == aaw_session::kMarkerHandle) {
        LOGE("wired retry: no usable DevMgr handle (captured=%d stored=0x%x saved=0x%x); "
             "not replaying", (int)g_real_dm_valid,
             cm ? AapConnectionManager_dev_handle(cm) : 0u, (unsigned)g_wired_dm_snap);
        reset_handoff_state("no DevMgr handle");
        free(r);
        return nullptr;
    }
    LOGE("wired retry: DevMgr handle 0x%x (%s)", handle, hsrc);
    // Retire the shared copies now so no later cable-in can reuse this handle.
    reset_handoff_state("handle consumed");

    unsigned char info[kDevInfoSize];
    memset(info, 0, sizeof info);
    uint32_t type  = kAaDeviceType;
    uint32_t vid32 = r->vid;
    memcpy(info + kOffType,   &type,   sizeof type);
    memcpy(info + kOffHandle, &handle, sizeof handle);
    memcpy(info + kOffVid,    &vid32,  sizeof vid32);
    snprintf((char *)(info + kOffName),   kStrFieldSz, "%s", name);
    snprintf((char *)(info + kOffSerial), kStrFieldSz, "%s", r->serial);

    LOGE("wired retry: session released after %dms; replaying device connect "
         "(vid=0x%x name=\"%.31s\" serial=\"%.31s\")",
         waited, (unsigned)r->vid, name, r->serial);
    int rc = AapConnectionManager_DevMgrEnabledCapCb(nullptr, info, nullptr);
    LOGE("wired retry: DevMgrEnabledCapCb -> %d", rc);

    pthread_t aw;
    if (preload_thread_create(&aw, wired_activate_watch, (void *)(intptr_t)r->gen) == 0)
        pthread_detach(aw);
    else
        LOGE("wired retry: could not spawn the activation watch");
    free(r);
    return nullptr;
}

} // namespace

namespace aaw_session {

void note_real_devmgr_handle(unsigned a1, unsigned handle)
{
    if (handle == kMarkerHandle) return;
    if (g_real_dm_valid && g_real_dm_handle == handle) return;
    g_real_dm_handle = handle;
    g_real_dm_valid  = true;
    LOGE("captured DevMgr handle 0x%x (a1=0x%x)", handle, a1);
}

void maybe_arm_wired_retry(int rc)
{
    if (!g_handoff_start_pending) return;
    g_handoff_start_pending = false;
    if (rc == 0) {
        // Accepted, so nothing will consume the saved handle.
        reset_handoff_state("OEM wired start succeeded");
        return;
    }
    if (rc != kStartRefused)
        LOGE("wired retry: OEM wired start returned %d (expected %d); arming anyway",
             rc, kStartRefused);

    WiredRetry *r = static_cast<WiredRetry *>(malloc(sizeof *r));
    if (!r) { LOGE("wired retry: out of memory"); return; }
    r->gen = g_session_gen;
    r->vid = g_handoff_vid;
    snprintf(r->serial, sizeof r->serial, "%s", g_handoff_serial);

    // The Device-Not-Responding watchdog would fire right as the retried
    // session connects. See replay_device_disconnect().
    void *cm = AapConnectionManager_instance();
    if (cm) {
        AapConnectionManager_StopDeviceNotRespondingTimer(cm);
    } else {
        LOGE("wired retry: AapConnectionManager unresolved; cannot stop the "
             "Device-Not-Responding watchdog");
    }

    pthread_t th;
    if (preload_thread_create(&th, wired_retry_main, r) == 0) {
        pthread_detach(th);
        LOGE("wired retry armed: OEM start refused (%d), retrying once the old "
             "session is released (cap %dms)", rc, kRetryCapMs);
    } else {
        free(r);
        LOGE("wired retry: could not spawn the retry thread");
    }
}

void note_wired_session(const char *serial, uint16_t vid)
{
    const char *s = (serial && serial[0]) ? serial : "AndroidAuto";
    if (!g_wireless_active) {
        LOGE("wired connect (serial=\"%.63s\", vid=0x%x)", s, (unsigned)vid);
        return;
    }

    // A wireless session can end without a WirelessAaDisconnected (the phone
    // drops Wi-Fi), leaving the flag set. If the SDK has nothing live, this is
    // a plain wired connect.
    uint32_t st0 = read_session_state();
    if (st0 == kSessInvalid) {
        g_wireless_active = false;
        LOGE("wired connect (serial=\"%.63s\", vid=0x%x); no live wireless session",
             s, (unsigned)vid);
        return;
    }

    // Handoff. Let the OEM's start run and record what the retry will need.
    // g_wireless_active stays set until the retry sees the session released.
    g_handoff_start_pending = true;
    snapshot_wired_dm_handle("wired start");
    g_handoff_vid = vid;
    snprintf(g_handoff_serial, sizeof g_handoff_serial, "%s", s);

    LOGE("wireless->wired handoff (state=0x%x, serial=\"%.63s\", vid=0x%x)",
         (unsigned)st0, s, (unsigned)vid);
}

void start()
{
    if (!libpatch_config::wireless_enabled()) {
        LOGD("wireless=false; session listener not started");
        return;
    }
    pthread_t th;
    if (preload_thread_create(&th, listener_main, nullptr) == 0) {
        pthread_detach(th);
        LOGD("session listener started");
    } else {
        LOGE("cannot start the session listener; wireless AA will not start");
    }
}

} // namespace aaw_session
