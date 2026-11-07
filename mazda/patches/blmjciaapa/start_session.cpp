// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libpatch-blmjciaapa.so — aap_start_session hook (wireless Android Auto).
//
// RaceAap::StartSession calls aap_start_session(handle, AAP_StartInfo*), an
// import from libaap_interface.so, so the preloaded definition below replaces
// it. The real one is resolved through libaap_interface.so directly: jciAAPA
// loads blmjciaapa.so without RTLD_GLOBAL, so RTLD_NEXT cannot reach it (see
// common/preload.h).
//
// The device-connect path that aaw_session replays can only build a USB
// session: StoreDeviceInfo always sets the transport word of the StartInfo's
// AAP_DeviceInfo to 1 (USB). aap_service then looks for the phone on the USB
// bus, fails, and tears the session down. Transport 2 (Wi-Fi) makes it bind
// the TCP connection the phone has already opened. So for the wireless device
// the word is set to 2 before the call.
//
// The wireless device is recognised by the marker VID (aaw_session.h), never by
// serial. Wired starts are reported to aaw_session for the wireless->wired
// handoff and passed through unchanged.
//
// With `wireless = false` this is a plain passthrough.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // dladdr() (used by config::load)
#endif

#define LOG_TAG "AASTART"
#include "log.h"
#include "common/config.h"
#include "common/preload.h"
#include "aaw_session.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdint.h>

namespace {

constexpr const char *kIfaceSoname = "libaap_interface.so";
constexpr const char *kIfacePath   = "/usr/lib/libaap_interface.so";

typedef int (*aap_start_session_fn)(void *a0, void *a1);

// ---- fault-safe read probe -------------------------------------------------
// write() from an unmapped address into a pipe fails with EFAULT instead of
// raising SIGSEGV. The byte is drained right away so the pipe never fills.
int g_rd = -1, g_wr = -1;

bool probe_init()
{
    if (g_rd >= 0) return true;
    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    g_rd = fds[0];
    g_wr = fds[1];
    return true;
}

bool byte_readable(const void *p)
{
    if (!probe_init()) return false;
    if (write(g_wr, p, 1) == 1) { char drain; (void)read(g_rd, &drain, 1); return true; }
    return false;
}

// Readable if the first byte of every page in the range, and its last byte, are.
bool region_readable(const void *p, size_t len)
{
    if (!p || !len) return false;
    const uintptr_t PG = 0x1000;
    uintptr_t a = (uintptr_t)p, end = a + len;
    for (uintptr_t cur = a; cur < end; cur = (cur & ~(PG - 1)) + PG)
        if (!byte_readable((const void *)cur)) return false;
    return byte_readable((const void *)(end - 1));
}

// AAP_StartInfo fields (the AAP_DeviceInfo starts at +0x14).
constexpr unsigned kDevTypeOff = 0x14;   // u32 transport: 1 = USB, 2 = Wi-Fi
constexpr unsigned kSerialOff  = 0x58;   // char[64] serial
constexpr unsigned kVidOff     = 0x98;   // u16 USB vendor id
constexpr uint32_t kTransportWiFi = 2;

void maybe_switch_to_wifi(void *a1)
{
    if (!a1 || !region_readable(a1, 0xa0)) return;
    uint16_t vid = 0;
    std::memcpy(&vid, (const char *)a1 + kVidOff, sizeof vid);
    const char *serial = (const char *)a1 + kSerialOff;
    if (vid != aaw_session::kMarkerVid) {
        aaw_session::note_wired_session(serial, vid);   // real VID: wired
        return;
    }
    uint32_t *type = (uint32_t *)((char *)a1 + kDevTypeOff);
    if (*type == kTransportWiFi) return;
    LOGE("wireless device (serial=%.63s): transport %u -> %u (Wi-Fi)",
         serial, (unsigned)*type, (unsigned)kTransportWiFi);
    // volatile: the OEM reads this word back after the call.
    *(volatile uint32_t *)type = kTransportWiFi;
}

} // namespace

extern "C" PRELOAD_EXPORT int aap_start_session(void *a0, void *a1)
{
    static aap_start_session_fn real = nullptr;
    static void *iface_handle = nullptr;
    if (!real) {
        real = (aap_start_session_fn)resolve_real_symbol(
            "aap_start_session", kIfaceSoname, kIfacePath,
            (void *)&aap_start_session, &iface_handle);
    }

    bool armed = false;
    try {
        libpatch_config::load((const void *)&aap_start_session);
        armed = libpatch_config::wireless_enabled();
    } catch (...) { armed = false; }

    if (armed)
        maybe_switch_to_wifi(a1);

    if (!real) {
        // Never recurse into ourselves. 0 is what the caller expects on success.
        LOGE("aap_start_session: real implementation unresolved");
        return 0;
    }

    const int rc = real(a0, a1);

    // Handoff only: arm the retry if the OEM's wired start was refused.
    if (armed)
        aaw_session::maybe_arm_wired_retry(rc);

    return rc;
}
