// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libpatch-blmjciaapa.so — device-manager settings for the wireless device.
//
// Once a session connects, AapConnectionManager::SelectConnectMode finalizes
// the device with eDevMgrGetDeviceSettings(a1, handle, &settings). That marks
// the device ready, stops the video-sink and "device not responding"
// watchdogs and shows the Android Auto start dialog. The real device manager
// has no record of the wireless device, which never enumerated on USB, so the
// query fails, the finalization is skipped, and ~85 s later the watchdog tears
// the session down while the menu entry stays disabled.
//
// The wireless device carries the marker handle (aaw_session.h). For it this
// returns success as a known device, which also skips the first-connection
// consent dialog. Every other handle goes to the genuine implementation in
// libdevmgr_interface.so, so wired Android Auto is unaffected.

#define LOG_TAG "DEVMGR"
#include "log.h"
#include "common/preload.h"
#include "aaw_session.h"

#include <cstdint>

namespace {

constexpr const char *kDevMgrSoname = "libdevmgr_interface.so";
constexpr const char *kDevMgrPath   = "/usr/lib/libdevmgr_interface.so";

typedef int (*edmgs_fn)(unsigned a1, unsigned a2, void *settings);

} // namespace

// Declared here so resolve_real_symbol can exclude it.
extern "C" PRELOAD_EXPORT int eDevMgrGetDeviceSettings(unsigned a1, unsigned a2, void *settings);

namespace {

edmgs_fn real_edmgs()
{
    static edmgs_fn real = nullptr;
    static void *handle = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        real = (edmgs_fn)resolve_real_symbol(
            "eDevMgrGetDeviceSettings", kDevMgrSoname, kDevMgrPath,
            (void *)&eDevMgrGetDeviceSettings, &handle);
        if (!real)
            LOGE("real eDevMgrGetDeviceSettings unresolved");
    }
    return real;
}

} // namespace

extern "C" PRELOAD_EXPORT int eDevMgrGetDeviceSettings(unsigned a1, unsigned a2, void *settings)
{
    if (a2 == aaw_session::kMarkerHandle) {
        if (settings) {
            uint32_t *s = static_cast<uint32_t *>(settings);
            s[0] = 1;        // settings valid
            // Connect mode. 1 = new device: show the "allow Android Auto?"
            // consent, which would appear on every connect because the device
            // manager cannot store the wireless device. 0x8000 = known device:
            // update the stored settings and check the video sink, no dialog.
            s[2] = 0x8000;
        }
        LOGE("eDevMgrGetDeviceSettings: wireless device -> known device");
        return 1;
    }
    // A real device: keep its handle for the wireless->wired handoff replay.
    aaw_session::note_real_devmgr_handle(a1, a2);
    edmgs_fn real = real_edmgs();
    return real ? real(a1, a2, settings) : 0xff;
}
