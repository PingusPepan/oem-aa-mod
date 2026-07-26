// In-place `key = value` rewriter for libpatch.conf.
//
// The shipped libpatch.conf is mostly the documentation for each key, so editing
// it from the head unit must not cost the user that: only the value token of each
// line that already sets the key is replaced, byte for byte everywhere else. A key
// the file never sets is appended under a marker comment.
//
// Every line setting the key is rewritten, not just the first: config.h applies
// them in order, so the last one wins, and leaving any of them stale could make a
// saved change have no effect.
//
// Parsing is not here — patches/common/config.h already does it (parse_file +
// apply_kv + the Settings defaults) and is what the shims use. This only writes.
//
// /data_persist is flash with deferred write-back, so: temp sibling, fsync,
// rename, fsync the directory. A power cut can lose the change but never leave a
// truncated libpatch.conf, which would silently reset every other key.

#ifndef OEM_AA_MOD_CONF_WRITE_H
#define OEM_AA_MOD_CONF_WRITE_H

#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <sys/stat.h>
#include <unistd.h>

namespace conf_write {

// The shipped file is ~11 KB. Anything larger is not ours: refuse rather than
// truncate.
static const size_t kMaxConfBytes = 64 * 1024;

// Static, not automatic: two 64 KB frames is a lot to ask of a thread whose stack
// the host sized, and this only runs on jciDbapi's single pluginManager thread.
inline char *read_buf()  { static char b[kMaxConfBytes]; return b; }
inline char *build_buf() { static char b[kMaxConfBytes]; return b; }

// True if `line` assigns `key`: first non-blank char starts the key, matched
// case-insensitively as config.h does, next non-blank char is '='. Comment lines
// never match, so a documented-but-disabled `# aa_bssid =` stays a comment.
inline bool line_sets_key(const char *line, size_t len, const char *key)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= len || line[i] == '#') {
        return false;
    }

    const size_t klen = strlen(key);
    if (len - i < klen || strncasecmp(line + i, key, klen) != 0) {
        return false;
    }
    i += klen;

    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return i < len && line[i] == '=';
}

// Keep everything through the '=' and its padding byte-for-byte, replace the rest
// with `value`; 0 if it would not fit. The remainder IS the value — config.h only
// treats '#' as a comment at the start of a line, so there are no trailing
// comments to preserve.
inline size_t rewrite_line(const char *line, size_t len, const char *value,
                           char *out, size_t out_sz)
{
    const char *eq = (const char *)memchr(line, '=', len);
    if (eq == NULL) {
        return 0;   // line_sets_key() guarantees one; belt and braces
    }

    size_t head = (size_t)(eq - line) + 1;                 // through the '='
    while (head < len && (line[head] == ' ' || line[head] == '\t')) {
        ++head;                                            // and its padding
    }

    if (head + strlen(value) >= out_sz) {
        return 0;
    }
    memcpy(out, line, head);
    strcpy(out + head, value);
    return head + strlen(value);
}

// fsync the directory holding `path`, so a rename() into it is durable.
inline void sync_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';

    const int fd = open(dir[0] ? dir : "/", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGW("conf: cannot open %s to fsync: %s", dir, strerror(errno));
        return;
    }
    if (fsync(fd) != 0) {
        LOGW("conf: fsync(%s) failed: %s", dir, strerror(errno));
    }
    close(fd);
}

// Set `key` to `value`, creating the file if absent. The caller must have
// validated both against the exposed schema; this does no vetting of its own.
inline bool set_key(const char *path, const char *key, const char *value)
{
    char  *buf      = read_buf();
    size_t used     = 0;
    bool   had_file = false;

    FILE *in = fopen(path, "re");
    if (in != NULL) {
        had_file = true;
        used     = fread(buf, 1, kMaxConfBytes, in);
        const bool overflowed = (used == kMaxConfBytes) && (fgetc(in) != EOF);
        fclose(in);
        if (overflowed) {
            LOGE("conf: %s is larger than %u bytes — refusing to rewrite it",
                 path, (unsigned)kMaxConfBytes);
            return false;
        }
    } else if (errno != ENOENT) {
        LOGE("conf: cannot read %s: %s", path, strerror(errno));
        return false;
    }

    // Rebuild line by line, substituting every line that assigns the key.
    char  *out      = build_buf();
    size_t olen     = 0;
    bool   replaced = false;

    size_t pos = 0;
    while (pos < used) {
        const char  *nl   = (const char *)memchr(buf + pos, '\n', used - pos);
        const size_t raw  = nl ? (size_t)(nl - (buf + pos)) + 1 : used - pos;
        size_t       body = nl ? raw - 1 : raw;   // without the '\n'
        if (body > 0 && buf[pos + body - 1] == '\r') {
            --body;                              // tolerate CRLF
        }

        char        repl[1024];
        const char *emit     = buf + pos;
        size_t      emit_len = raw;

        if (line_sets_key(buf + pos, body, key)) {
            const size_t n =
                rewrite_line(buf + pos, body, value, repl, sizeof(repl));
            if (n == 0) {
                LOGE("conf: %s line for \"%s\" too long to rewrite", path, key);
                return false;
            }
            // Re-attach whatever line terminator the original line had.
            size_t m = n;
            for (size_t k = body; k < raw && m + 1 < sizeof(repl); ++k) {
                repl[m++] = buf[pos + k];
            }
            repl[m] = '\0';
            emit     = repl;
            emit_len = m;
            replaced = true;
        }

        if (olen + emit_len > kMaxConfBytes) {
            LOGE("conf: rewrite of %s would exceed %u bytes", path,
                 (unsigned)kMaxConfBytes);
            return false;
        }
        memcpy(out + olen, emit, emit_len);
        olen += emit_len;
        pos  += raw;
    }

    if (!replaced) {
        // Not set anywhere (absent, or only as a comment): append under a marker,
        // which a later edit then finds and rewrites in place.
        const char *hdr = had_file
            ? "\n# --- set from the head unit (Settings > Android Auto Mod) ---\n"
            : "# libpatch.conf — written from the head unit\n"
              "# (Settings > System > Android Auto Mod). Keys not listed here\n"
              "# keep their built-in defaults.\n";
        char tail[1024];
        const int n = snprintf(tail, sizeof(tail), "%s%s = %s\n", hdr, key, value);
        if (n < 0 || (size_t)n >= sizeof(tail) ||
            olen + (size_t)n > kMaxConfBytes) {
            LOGE("conf: no room to append \"%s\" to %s", key, path);
            return false;
        }
        memcpy(out + olen, tail, (size_t)n);
        olen += (size_t)n;
    }

    // Atomic replace: temp sibling -> fsync -> rename -> fsync dir.
    char tmp[512];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        LOGE("conf: path %s too long", path);
        return false;
    }

    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOGE("conf: cannot create %s: %s", tmp, strerror(errno));
        return false;
    }

    size_t written = 0;
    while (written < olen) {
        const ssize_t w = write(fd, out + written, olen - written);
        if (w <= 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("conf: write to %s failed: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return false;
        }
        written += (size_t)w;
    }
    if (fsync(fd) != 0) {
        LOGE("conf: fsync(%s) failed: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return false;
    }
    close(fd);

    if (rename(tmp, path) != 0) {
        LOGE("conf: rename(%s -> %s) failed: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return false;
    }
    sync_parent_dir(path);

    LOGD("conf: %s -> %s = %s (%s)", path, key, value,
         replaced ? "rewrote existing line(s)" : "appended");
    return true;
}

} // namespace conf_write

#endif // OEM_AA_MOD_CONF_WRITE_H
