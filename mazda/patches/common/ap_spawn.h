// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ap_spawn.h — start a shell script fully detached from the calling process.
//
// Used by libpatch-aap_service (aa_ap_script) to start the wireless AA AP
// script without an sm.conf service. The child gets its own session, stdin
// from /dev/null, stdout/stderr to `log_path`, no inherited descriptors above
// 2 and a fixed, clean environment (no LD_PRELOAD, so nothing it starts loads
// the shim). The double fork means the caller never has a zombie and never
// waits for the script. Only async-signal-safe calls run between fork and
// exec, so it is safe in a multithreaded process.
//
// Header-only, like common/config.h, so test/ap_spawn_test.cpp can exercise
// the same code on the build host.

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ap_spawn {

// Returns the pid of the intermediate child (already reaped), or -1 when
// nothing was started (empty/unreadable script or fork failure).
inline pid_t start_detached(const char *script, const char *log_path)
{
    if (!script || !script[0] || access(script, R_OK) != 0) return -1;
    static char *const envp[] = {
        const_cast<char *>("PATH=/usr/sbin:/usr/bin:/sbin:/bin"),
        const_cast<char *>("HOME=/"),
        nullptr
    };
    char *const argv[] = { const_cast<char *>("sh"), const_cast<char *>(script), nullptr };
    const pid_t first = fork();
    if (first < 0) return -1;
    if (first == 0) {
        setsid();
        const pid_t second = fork();
        if (second == 0) {
            const int nul = open("/dev/null", O_RDONLY);
            const int out = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (nul >= 0) dup2(nul, 0);
            if (out >= 0) { dup2(out, 1); dup2(out, 2); }
            for (int fd = 3; fd < 256; ++fd) close(fd);
            execve("/bin/sh", argv, envp);
            _exit(127);
        }
        _exit(second < 0 ? 1 : 0);
    }
    int status = 0;
    while (waitpid(first, &status, 0) < 0) {
        // EINTR: retry; anything else: give up waiting, the child exits anyway.
        if (errno != EINTR) break;
    }
    return first;
}

} // namespace ap_spawn
