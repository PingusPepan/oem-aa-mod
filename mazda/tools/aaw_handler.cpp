// SPDX-License-Identifier: AGPL-3.0-or-later
//
// aaw_handler — Android Auto Wireless Bluetooth handshake.
//
// bds advertises the "Android Auto Wireless" RFCOMM service
// (BdsConfiguration.xml, serialPort id 15), so the phone offers wireless
// Android Auto. When the phone connects to it, bds bridges the channel to a PTY
// and BCA announces its path. This program runs the AAW handshake (aaw.h) on
// that PTY: it gives the phone the AP credentials and the AA TCP endpoint, the
// phone joins the AP and connects to aap_service, and once the phone reports
// success it emits com.jci.aaw.WirelessAaConnected so jciAAPA starts the
// session.
//
// Settings come from libpatch.conf, the same keys the aap_service shim uses:
// aa_ssid, aa_psk, aa_ap_ip, aa_tcp_port, aa_security, aa_iface, aa_bssid
// (empty = the live MAC of aa_iface).
//
// Usage:  aaw_handler <pty> [libpatch.conf] [bt-device-id]
//   <pty>            the PTY BCA announced for the AAW channel
//   [libpatch.conf]  default /data_persist/oem-aa-mod/libpatch.conf
//   [bt-device-id]   BCA's device id of the phone on this channel; passed on
//                    to jciAAPA
// aaw_listend starts one per connection. To run it by hand, take the PTY path
// and device id from BCA's connection signal.
//
// Exit status (aaw.h HandlerExit) tells aaw_listend whether the phone found
// the AP.

#include "aaw.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <stdint.h>
#include <dbus/dbus.h>

namespace {

struct Creds {
    char     ssid[64];
    char     psk[80];
    char     ap_ip[24];
    char     bssid[24];
    char     iface[16];
    unsigned port;
    unsigned security;
};

void log_ts(const char *fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%ld aaw_handler: %s\n", (long)time(nullptr), line);
    fflush(stdout);
}

// Minimal `key = value` reader for libpatch.conf. '#' starts a comment line;
// whitespace around keys and values is ignored.
void load_creds(const char *path, Creds &c)
{
    // Defaults, as in common/config.h.
    snprintf(c.ssid, sizeof c.ssid, "%s", "MazdaAA");
    snprintf(c.psk, sizeof c.psk, "%s", "MazdaAndroidAuto");
    snprintf(c.ap_ip, sizeof c.ap_ip, "%s", "192.168.53.1");
    c.bssid[0] = '\0';
    snprintf(c.iface, sizeof c.iface, "%s", "wlan1");
    c.port = 5277;
    c.security = aaw::WPA2_PERSONAL;

    FILE *f = fopen(path, "re");
    if (!f) { log_ts("WARN: %s not found, using defaults", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq + 1;
        char *ke = eq; while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) --ke; *ke = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) --ve;
        *ve = '\0';
        if      (!strcasecmp(key, "aa_ssid"))     snprintf(c.ssid, sizeof c.ssid, "%s", val);
        else if (!strcasecmp(key, "aa_psk"))      snprintf(c.psk, sizeof c.psk, "%s", val);
        else if (!strcasecmp(key, "aa_ap_ip"))    snprintf(c.ap_ip, sizeof c.ap_ip, "%s", val);
        else if (!strcasecmp(key, "aa_bssid"))    snprintf(c.bssid, sizeof c.bssid, "%s", val);
        else if (!strcasecmp(key, "aa_iface"))    snprintf(c.iface, sizeof c.iface, "%s", val);
        else if (!strcasecmp(key, "aa_tcp_port")) c.port = (unsigned)strtoul(val, nullptr, 10);
        else if (!strcasecmp(key, "aa_security")) c.security = (unsigned)strtoul(val, nullptr, 10);
    }
    fclose(f);
}

// BSSID to send: aa_bssid, else the live MAC of aa_iface.
const char *resolve_bssid(Creds &c)
{
    if (c.bssid[0]) return c.bssid;
    char path[64];
    snprintf(path, sizeof path, "/sys/class/net/%s/address", c.iface);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    int n = (int)read(fd, c.bssid, sizeof(c.bssid) - 1);
    close(fd);
    if (n <= 0) { c.bssid[0] = '\0'; return ""; }
    while (n > 0 && (c.bssid[n-1] == '\n' || c.bssid[n-1] == '\r' || c.bssid[n-1] == ' ')) --n;
    c.bssid[n] = '\0';
    return c.bssid;
}

bool send_msg(int fd, uint16_t msg_id, const aaw::Writer &w, const char *what)
{
    uint8_t out[600];
    unsigned n = aaw::frame(out, sizeof out, msg_id, w.buf, w.len);
    if (!n) { log_ts("ERR: %s does not fit", what); return false; }
    ssize_t wr = write(fd, out, n);
    log_ts("TX %s (msgId=%u, %u B): %s", what, msg_id, n, wr == (ssize_t)n ? "ok" : "short/err");
    return wr == (ssize_t)n;
}

// The phone's serial from WifiVersionResponse field 3; the same serial the
// wired path reports. Passed on with WirelessAaConnected.
char g_serial[64] = {0};

// BCA's device id of the phone on this channel (argv[3]). Passed on with the
// serial so jciAAPA knows which paired phone this session is.
uint32_t g_bt_dev = 0;

// The phone could not find the AP (see aaw.h Status). aaw_listend resets the
// dongle on it.
bool g_ap_unreachable = false;
bool g_reported_success = false;   // the phone joined and connected

// ---- silent-radio check ------------------------------------------------------
// After the phone acknowledges WifiStartRequest it has to scan, find the AP and
// associate, which takes about 8 s. If the radio is silent the phone only
// reports WIFI_NETWORK_UNAVAILABLE after its own 36 s join timeout. This check
// reaches the same verdict sooner by watching the AP's rx_packets.
//
// rx_packets counts data frames only; probe, authentication and association
// frames are not counted. It therefore starts climbing only once a station has
// associated (EAPOL is the first data frame), not when the radio first hears
// something.
//
// - The window is the same for every attempt and must exceed the association
//   time. After a dongle reset the AP is a new BSS that the phone must find
//   from scratch, which takes as long as a first connect. A shorter window
//   declares healthy APs dead, and each false reset destroys the AP the phone
//   was about to join. Override with AAW_DEAF_WINDOW.
// - The window starts only once the AP can be joined: the driver's fwstate has
//   WIFI_AP_STATE set and WIFI_SITE_MONITOR clear. Right after a reset hostapd
//   may still be bringing the BSS up.
// - The verdict is the increase at the end of the window. A working link
//   climbs by dozens of frames within a second or two of associating; a
//   handful means association has barely begun or is failing.
const time_t kDeafWindow = 20;   // s; well above the ~8 s association time
const long long kAliveDelta  = 10;

time_t g_deaf_window = kDeafWindow;   // AAW_DEAF_WINDOW

long long g_rx_at_ack   = -1;      // rx_packets at the window start; -1 = not armed
time_t    g_ack_time    = 0;
bool      g_deaf_called = false;   // reported once
bool      g_saw_alive   = false;   // this AP has received data

long long read_rx_packets(const char *iface)
{
    char path[128];
    snprintf(path, sizeof path, "/sys/class/net/%s/statistics/rx_packets", iface);
    int rfd = open(path, O_RDONLY);
    if (rfd < 0) return -1;
    char b[32];
    int n = (int)read(rfd, b, sizeof b - 1);
    close(rfd);
    if (n <= 0) return -1;
    b[n] = '\0';
    return strtoll(b, nullptr, 10);
}

// Can the AP be joined yet? fwstate has WIFI_AP_STATE (0x10) once the BSS is up
// and WIFI_SITE_MONITOR (0x800) while it is still scanning. If fwstate cannot
// be read (another driver, or no proc-debug), assume ready.
bool ap_is_ready(const char *iface)
{
    char link[256] = {0}, path[256];
    snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", iface);
    int n = (int)readlink(path, link, sizeof link - 1);
    if (n <= 0) return true;
    link[n] = '\0';
    const char *drv = strrchr(link, '/');
    drv = drv ? drv + 1 : link;

    snprintf(path, sizeof path, "/proc/net/%s/%s/fwstate", drv, iface);
    FILE *f = fopen(path, "r");
    if (!f) return true;
    char buf[64] = {0};
    bool got = (fgets(buf, sizeof buf, f) != nullptr);
    fclose(f);
    if (!got) return true;
    const char *eq = strchr(buf, '=');
    if (!eq) return true;
    unsigned long fw = strtoul(eq + 1, nullptr, 0);
    return (fw & 0x10) && !(fw & 0x800);
}

// True once the window has passed without enough received data. Disarms as
// soon as the counter climbs by kAliveDelta.
bool radio_is_deaf(const Creds &c)
{
    if (g_rx_at_ack < 0 || g_deaf_called) return false;
    long long rx = read_rx_packets(c.iface);
    if (rx < 0) return false;
    // Restart the window until the AP can be joined.
    if (!ap_is_ready(c.iface)) {
        static bool said = false;
        if (!said) { said = true;
            log_ts("radio-check: %s not joinable yet; window starts when it is", c.iface); }
        g_ack_time  = time(nullptr);
        g_rx_at_ack = rx;
        return false;
    }
    long elapsed = (long)(time(nullptr) - g_ack_time);
    long long delta = rx - g_rx_at_ack;

    if (delta >= kAliveDelta) {
        log_ts("radio-check: %s rx_packets +%lld by t+%lds, the AP is on the air",
               c.iface, delta, elapsed);
        g_saw_alive = true;
        g_rx_at_ack = -1;
        return false;
    }
    if (elapsed < g_deaf_window) return false;

    log_ts("radio-check: %s rx_packets only +%lld (now %lld) after %lds; reporting the AP "
           "unreachable", c.iface, delta, rx, elapsed);
    g_deaf_called = true;
    return true;
}

// Copy length-delimited field `tag` (tag byte) into out, NUL-terminated and
// truncated to outsz. One-byte lengths only; AAW strings are short.
bool pb_get_ld(const uint8_t *buf, unsigned len, uint8_t tag, char *out, unsigned outsz)
{
    unsigned i = 0;
    while (i < len) {
        uint8_t t = buf[i++];
        unsigned wt = t & 7;
        if (t == tag && wt == 2) {
            if (i >= len) return false;
            unsigned l = buf[i++];
            if (l > len - i) l = len - i;
            unsigned n = l < outsz - 1 ? l : outsz - 1;
            memcpy(out, buf + i, n); out[n] = '\0';
            return true;
        }
        if (wt == 0) { while (i < len && (buf[i++] & 0x80)) {} }
        else if (wt == 2) { if (i >= len) break; i += (unsigned)buf[i] + 1; }
        else if (wt == 5) i += 4;
        else if (wt == 1) i += 8;
        else break;
    }
    return false;
}

// ---- AA TCP gate ------------------------------------------------------------
// WifiConnectionStatus=SUCCESS means the phone has joined the AP and is
// connecting to the AA TCP server, not that the connection exists. If the
// session starts before aap_service has accepted the socket, the transport
// finds no connection ("TCP connection is not yet established", "Failed to
// start transport layer"), the session is destroyed within a second, and
// nothing retries until the phone drops Bluetooth. Even when it works the
// margin can be as small as ~20 ms.
//
// So the session is announced only once /proc/net/tcp shows an ESTABLISHED
// (01) socket on the AA port; the listener itself is in LISTEN (0A).
const int kTcpWaitMs = 10000;   // cap; normally well under 1 s
const int kTcpPollMs = 100;

bool aa_tcp_established(unsigned port)
{
    FILE *f = fopen("/proc/net/tcp", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    if (fgets(line, sizeof line, f)) {                 // header row
        while (!found && fgets(line, sizeof line, f)) {
            unsigned lport = 0, st = 0;
            //  sl: local_addr:PORT rem_addr:PORT ST ...
            if (sscanf(line, "%*u: %*8x:%x %*8x:%*x %x", &lport, &st) == 2 &&
                lport == port && st == 1)
                found = true;
        }
    }
    fclose(f);
    return found;
}

// Bounded wait. On timeout the session is announced anyway, which is no worse
// than not waiting, but it will most likely fail.
void wait_for_aa_tcp(unsigned port)
{
    for (int waited = 0; waited <= kTcpWaitMs; waited += kTcpPollMs) {
        if (aa_tcp_established(port)) {
            log_ts("aa-tcp: connection on port %u established after %d ms", port, waited);
            return;
        }
        usleep((useconds_t)kTcpPollMs * 1000);
    }
    log_ts("aa-tcp: WARNING no connection on port %u after %d ms; starting the session "
           "anyway, it will probably fail", port, kTcpWaitMs);
}

// Tell jciAAPA to start the session (libpatch-blmjciaapa.so listens for this on
// the service bus; see aaw_session.cpp). Arguments: the phone's serial and
// BCA's device id of the phone.
void emit_wireless_connected(const char *serial)
{
    DBusError err; dbus_error_init(&err);
    DBusConnection *conn =
        dbus_connection_open_private("unix:path=/tmp/dbus_service_socket", &err);
    if (!conn) {
        log_ts("dbus: open service bus failed: %s",
               dbus_error_is_set(&err) ? err.message : "?");
        dbus_error_free(&err);
        return;
    }
    if (!dbus_bus_register(conn, &err)) {
        log_ts("dbus: bus_register failed: %s",
               dbus_error_is_set(&err) ? err.message : "?");
        dbus_error_free(&err);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }
    DBusMessage *sig = dbus_message_new_signal("/com/jci/aaw", "com.jci.aaw",
                                               "WirelessAaConnected");
    if (sig) {
        const char *s = serial ? serial : "";
        dbus_uint32_t dev = g_bt_dev;
        dbus_message_append_args(sig, DBUS_TYPE_STRING, &s, DBUS_TYPE_UINT32, &dev,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, sig, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(sig);
        log_ts("dbus: emitted com.jci.aaw.WirelessAaConnected serial=\"%s\" btDev=%u",
               s, (unsigned)dev);
    } else {
        log_ts("dbus: dbus_message_new_signal returned NULL");
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pty> [libpatch.conf] [bt-device-id]\n", argv[0]);
        return 2;
    }
    if (const char *w = getenv("AAW_DEAF_WINDOW")) {
        long v = strtol(w, nullptr, 10);
        if (v > 0) g_deaf_window = (time_t)v;
    }
    const char *pty  = argv[1];
    const char *conf = argc > 2 ? argv[2] : "/data_persist/oem-aa-mod/libpatch.conf";
    if (argc > 3) g_bt_dev = (uint32_t)strtoul(argv[3], nullptr, 10);

    Creds c;
    load_creds(conf, c);
    const char *bssid = resolve_bssid(c);
    log_ts("creds: ssid=\"%s\" bssid=%s ap=%s:%u sec=%u (conf=%s)",
           c.ssid, bssid[0] ? bssid : "(interface missing)", c.ap_ip, c.port, c.security, conf);

    int fd = open(pty, O_RDWR | O_NOCTTY);
    if (fd < 0) { log_ts("ERR: open(%s) failed", pty); return 1; }
    // Raw mode: this is an RFCOMM bridge, not a terminal.
    struct termios t;
    if (tcgetattr(fd, &t) == 0) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
    log_ts("opened %s, starting the AAW handshake", pty);

    // The phone sends nothing until the head unit speaks, and ignores a
    // WifiStartRequest as the first message. Open with the (empty)
    // WifiVersionRequest; WifiStartRequest follows the phone's answer.
    {
        aaw::Writer w;
        send_msg(fd, aaw::WIFI_VERSION_REQUEST, w, "WifiVersionRequest");
    }

    // Answer whatever the phone sends, in any order.
    uint8_t buf[2048];
    unsigned have = 0;
    for (;;) {
        // select() with a 1 s timeout so idle seconds run the radio check.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;          // reset every pass: select() modifies it
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        int sr = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sr < 0) {
            if (errno == EINTR) continue;
            log_ts("select error (%s); exiting", strerror(errno));
            break;
        }
        if (sr == 0) {
            if (radio_is_deaf(c)) { g_ap_unreachable = true; break; }
            continue;
        }
        ssize_t r = read(fd, buf + have, sizeof(buf) - have);
        if (r < 0) { log_ts("read error; exiting"); break; }
        if (r == 0) { log_ts("EOF (RFCOMM closed); exiting"); break; }
        have += (unsigned)r;

        unsigned off = 0;
        for (;;) {
            uint16_t id; unsigned plen;
            const uint8_t *payload = aaw::parse(buf + off, have - off, &id, &plen);
            if (!payload) break;                       // incomplete frame
            unsigned consumed = (unsigned)(payload - (buf + off)) + plen;
            {
                char hexs[160]; unsigned hn = 0;
                for (unsigned i = 0; i < plen && hn + 3 < sizeof(hexs); ++i)
                    hn += (unsigned)snprintf(hexs + hn, sizeof(hexs) - hn, "%02x ", payload[i]);
                log_ts("RX msgId=%u (%u B): %s", id, plen, hexs);
            }

            if (id == aaw::WIFI_VERSION_RESPONSE) {
                // Field 3: the phone's serial.
                if (pb_get_ld(payload, plen, 0x1a, g_serial, sizeof g_serial))
                    log_ts("phone serial = \"%s\"", g_serial);
                aaw::Writer w; aaw::build_wifi_start_request(w, c.ap_ip, c.port);
                send_msg(fd, aaw::WIFI_START_REQUEST, w, "WifiStartRequest");
            } else if (id == aaw::WIFI_VERSION_REQUEST) {
                // The phone asked first; answer it.
                aaw::Writer w; aaw::build_wifi_version_response(w);
                send_msg(fd, aaw::WIFI_VERSION_RESPONSE, w, "WifiVersionResponse");
            } else if (id == aaw::WIFI_INFO_REQUEST) {
                // An open AP gets an empty password; a stray one can confuse
                // some phones.
                const char *pw = (c.security == aaw::OPEN) ? "" : c.psk;
                aaw::Writer w; aaw::build_wifi_info_response(w, c.ssid, pw, bssid, c.security);
                send_msg(fd, aaw::WIFI_INFO_RESPONSE, w, "WifiInfoResponse");
            } else if (id == aaw::WIFI_START_REQUEST) {
                // Reconnect after the user disconnected on the phone. The RFCOMM
                // channel stays open, so aaw_listend starts no new handler;
                // instead the phone keeps sending WIFI_START_REQUEST (normally
                // head unit -> phone). Answer by restarting the handshake, at
                // most once every 4 s, since the phone sends these in bursts.
                static time_t last_redrive = 0;
                time_t now = time(nullptr);
                if (now - last_redrive >= 4) {
                    last_redrive = now;
                    log_ts("phone requested a restart; restarting the handshake");
                    aaw::Writer w;
                    send_msg(fd, aaw::WIFI_VERSION_REQUEST, w, "WifiVersionRequest (restart)");
                } else {
                    log_ts("phone requested a restart (handshake already running)");
                }
            } else if (id == aaw::WIFI_START_RESPONSE) {
                log_ts("phone acknowledged WifiStartRequest; joining %s, then TCP %s:%u",
                       c.ssid, c.ap_ip, c.port);
                // The phone starts scanning now: arm the radio check.
                g_rx_at_ack = read_rx_packets(c.iface);
                g_ack_time  = time(nullptr);
                g_deaf_called = false;
                log_ts("radio-check: armed on %s (rx_packets=%lld, need +%lld within %lds)",
                       c.iface, g_rx_at_ack, kAliveDelta, (long)g_deaf_window);
            } else if (id == aaw::WIFI_CONNECTION_STATUS) {
                // { required Status status = 1; optional string error_message = 2 }
                // Only SUCCESS starts the session; a missing or unreadable status
                // does not. The failure code matters: -3 = wrong credentials,
                // -10 = the phone's Wi-Fi is off, -11 = the phone did not find
                // the AP.
                int64_t st = 0;
                bool have = aaw::pb_varint_s(payload, plen, 1, &st);
                char errmsg[128] = {0};
                pb_get_ld(payload, plen, 0x12, errmsg, sizeof errmsg);
                bool ok = have && st == aaw::STATUS_SUCCESS;
                log_ts("phone reports WifiConnectionStatus -> %s (status=%s (%ld), serial=\"%s\")%s%s",
                       ok ? "SUCCESS" : "failure",
                       have ? aaw::status_name((int)st) : "FIELD-MISSING",
                       have ? (long)st : 0L, g_serial,
                       errmsg[0] ? " error_message=" : "", errmsg);
                g_rx_at_ack = -1;        // the phone answered; stop the radio check
                // WIFI_NETWORK_UNAVAILABLE while this AP is receiving data refers to
                // an earlier AP instance that a dongle reset replaced; acting on it
                // would reset a working radio.
                if (have && st == aaw::STATUS_WIFI_NETWORK_UNAVAILABLE && g_saw_alive) {
                    log_ts("radio-check: ignoring WIFI_NETWORK_UNAVAILABLE, %s is receiving "
                           "data", c.iface);
                } else {
                    g_ap_unreachable = have && st == aaw::STATUS_WIFI_NETWORK_UNAVAILABLE;
                }
                if (ok) {
                    g_reported_success = true;
                    wait_for_aa_tcp(c.port);
                    emit_wireless_connected(g_serial);
                }
            } else {
                log_ts("(unhandled msgId=%u)", id);
            }
            off += consumed;
        }
        // Keep any partial frame.
        if (off > 0) { memmove(buf, buf + off, have - off); have -= off; }
        if (have == sizeof(buf)) { log_ts("buffer full without a frame; discarding"); have = 0; }
    }

    close(fd);
    // Verdict for aaw_listend.
    if (g_ap_unreachable)   return aaw::HANDLER_AP_UNREACHABLE;
    if (g_reported_success) return aaw::HANDLER_SESSION_OK;
    return aaw::HANDLER_OK;
}
