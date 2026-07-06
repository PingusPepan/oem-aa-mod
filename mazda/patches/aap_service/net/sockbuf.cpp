// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sockbuf.cpp — keep the kernel's receive buffer on the Android Auto TCP socket.
//
// aap_service's transport configuration has a receive window of 0, which it
// passes to setsockopt(SO_RCVBUF, 0) on the AA TCP listener. The kernel raises
// that to its minimum, 2232 bytes, and the accepted socket that carries the
// projected video inherits it:
//
//   TRANS_TCP_LISTENER::...::Setting recv buffer size to 0
//   TRANS_TCP_LISTENER::...::Got recv buffer size as 2232
//
// Linux reserves about half of SO_RCVBUF for overhead, so the advertised window
// is ~1.1 KB. At a 3 ms round trip that caps throughput at ~2.9 Mbps; with
// ~13 KB per 800x480 frame that is ~28 fps at best. In practice Wi-Fi delivered
// 23-31 fps against 48-51 fps over USB, on an otherwise idle, loss-free link.
// With the buffer left alone, Wi-Fi reaches 49-50 fps.
//
// The call is skipped rather than given a larger value: any explicit SO_RCVBUF
// disables the kernel's receive-buffer autotuning for that socket. Without it
// the kernel sizes the window to the link, and Recv-Q stays at zero.
//
// SO_SNDBUF is set the same way and handled the same way.
//
// Only requests below kSaneFloor are skipped; the caller is told the call
// succeeded, and the socket keeps a larger buffer than requested, never a
// smaller one. USB sessions carry no TCP, so they are unaffected.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // RTLD_NEXT
#endif

#define LOG_TAG "SOCKBUF"
#include "../log.h"
#include "sockbuf.h"
#include "common/preload.h"

#include <sys/socket.h>
#include <sys/types.h>

// Declared here so resolve_real_symbol can exclude it.
extern "C" PRELOAD_EXPORT
int setsockopt(int fd, int level, int optname, const void *optval,
               socklen_t optlen);

namespace {

// Requests below this are skipped. 64 KiB is the largest TCP window without
// window scaling.
constexpr int kSaneFloor = 65536;

using setsockopt_real_fn = int (*)(int, int, int, const void *, socklen_t);

void *g_libc_handle = nullptr;

setsockopt_real_fn real_setsockopt()
{
    static setsockopt_real_fn fn = reinterpret_cast<setsockopt_real_fn>(
        resolve_real_symbol("setsockopt", "libc.so.6", nullptr,
                            reinterpret_cast<void *>(&setsockopt),
                            &g_libc_handle));
    return fn;
}

const char *optname_str(int optname)
{
    switch (optname) {
    case SO_RCVBUF: return "SO_RCVBUF";
    case SO_SNDBUF: return "SO_SNDBUF";
    default:        return "?";
    }
}

} // namespace

namespace aap_service_net {

void init()
{
    LOGD("sockbuf: SO_RCVBUF/SO_SNDBUF requests below %d bytes are skipped",
         kSaneFloor);
}

} // namespace aap_service_net

// ---- interposer ------------------------------------------------------------

extern "C" PRELOAD_EXPORT
int setsockopt(int fd, int level, int optname, const void *optval,
               socklen_t optlen)
{
    if (level == SOL_SOCKET &&
        (optname == SO_RCVBUF || optname == SO_SNDBUF) &&
        optval != nullptr && optlen >= sizeof(int)) {

        int want = 0;
        __builtin_memcpy(&want, optval, sizeof(want));

        if (want < kSaneFloor) {
            LOGE("skipped %s=%d on fd=%d (below %d)",
                 optname_str(optname), want, fd, kSaneFloor);
            return 0;
        }

        LOGD("passing through %s=%d on fd=%d", optname_str(optname), want, fd);
    }

    setsockopt_real_fn real = real_setsockopt();
    if (!real) {
        LOGE("setsockopt: real impl unresolved — cannot chain");
        return -1;
    }
    return real(fd, level, optname, optval, optlen);
}
