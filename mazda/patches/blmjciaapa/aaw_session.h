// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libpatch-blmjciaapa.so — wireless Android Auto session start/stop.
//
// See aaw_session.cpp for the mechanism.

#ifndef LIBPATCH_BLMJCIAAPA_AAW_SESSION_H
#define LIBPATCH_BLMJCIAAPA_AAW_SESSION_H

#include <stdint.h>

namespace aaw_session {

// Spawn the session listener thread. No-op unless `wireless = true`. Call once
// from the library constructor, after libpatch_config::load(). Safe before
// blmjciaapa.so is fully mapped: OEM addresses are resolved on first use.
void start();

// Transport marker. The OEM device-manager path only builds USB sessions, so
// the wireless device is replayed through it and its transport is switched to
// Wi-Fi in the aap_start_session hook. The two must be told apart by transport,
// not by serial: the same phone reports the same serial wired and wireless. The
// replayed device carries this value in its USB vendor-id field, which no real
// device reports; a wired session carries the phone's real VID and is never
// switched.
constexpr unsigned short kMarkerVid = 0xAA77;

// Device-handle marker. Once the session connects, the OEM finalizes the device
// with eDevMgrGetDeviceSettings(handle), which the real device manager fails for
// a device it never enumerated; the "device not responding" watchdog then tears
// the session down. The replayed device carries this handle, and
// devmgr_settings.cpp answers the query for it. Real handles go to the genuine
// implementation.
constexpr unsigned kMarkerHandle = 0xAA77AA77u;

// Called from the aap_start_session hook for every wired (real-VID) session
// start, with that phone's serial and VID. Never blocks and never swallows the
// start. On a wireless->wired handoff it records the phone's identity and the
// OEM's DevMgr handle for a later replay and flags the start, so that
// maybe_arm_wired_retry() can act on its result.
void note_wired_session(const char *serial, uint16_t vid);

// Called from the aap_start_session hook right after the real call returns.
// On a handoff the OEM's only wired start arrives while the wireless session is
// still releasing and is refused with 279; the OEM never retries. This arms a
// side thread that waits for the old session to reach INVALID and then replays
// the OEM's own device-connect (DevMgrEnabledCapCb), which runs the complete
// bring-up in the correct order. No-op for any other start.
void maybe_arm_wired_retry(int rc);

// Called from the eDevMgrGetDeviceSettings hook for every real (non-marker)
// device, with the handle the OEM queried. The handoff replay needs it.
void note_real_devmgr_handle(unsigned a1, unsigned handle);

} // namespace aaw_session

#endif  // LIBPATCH_BLMJCIAAPA_AAW_SESSION_H
