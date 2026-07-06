// SPDX-License-Identifier: AGPL-3.0-or-later
//
// aaw_listend — Android Auto Wireless connection supervisor.
//
// When the phone connects to the AAW RFCOMM service (BdsConfiguration.xml,
// serialPort id 15), bds bridges the channel to a PTY and BCA announces it.
// BCA handles service 15 in its wireless-projection state machine, so it emits
//
//     com.jci.bca.WcpConnectionStatusResp(connStatus u, btDeviceId u,
//                                         terminalPath (ay))
//
// rather than the generic ConnectionStatusResp, and it emits it on the HMI bus
// (/tmp/dbus_hmi_socket), not the service bus. This daemon therefore listens to
// all com.jci.bca signals on both buses and accepts either signal.
//
// - Non-empty terminalPath: run `aaw_handler <pty> <conf> <btDeviceId>` for the
//   handshake (one at a time).
// - Empty terminalPath: the channel is gone. Emit
//   com.jci.aaw.WirelessAaDisconnected so jciAAPA ends the session, and stop
//   the handler.
// - The handler reports that the phone could not find the AP, or the dongle
//   re-enumerates (USB power cut in standby): re-probe the dongle with
//   reset-aa-dongle.sh (see "AP recovery").
//
// Started at boot by start-aa-ap.sh. Logs to stdout.
//
// Usage:  aaw_listend [libpatch.conf]
//   argv[1]          libpatch.conf, passed on to aaw_handler
//                    (default /data_persist/oem-aa-mod/libpatch.conf)
// Environment:
//   AAW_HANDLER      aaw_handler path (default /data_persist/oem-aa-mod/aaw_handler)
//   AAW_SERVICE_ID   BCA service id of "Android Auto Wireless" (default 15)
//   AAW_RESET        recovery script (default /data_persist/wl/reset-aa-dongle.sh);
//                    if missing or not executable, recovery is disabled
//   AAW_RESET_AFTER  verdicts in a row before a reset (default 1)
//   AAW_IFACE        AP interface (default: aa_iface from libpatch.conf)
//   AAW_HMI_BUS / AAW_BUS_ADDRESS   D-Bus addresses (HMI / service bus)

#include "aaw.h"

#include <dbus/dbus.h>

#include <dirent.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

namespace {

const char *kHmiSock        = "unix:path=/tmp/dbus_hmi_socket";       // BCA status signals
const char *kHmiTcp         = "tcp:host=localhost,port=3001";
const char *kServiceSock    = "unix:path=/tmp/dbus_service_socket";
const char *kServiceTcp     = "tcp:host=localhost,port=3000";
const char *kDefaultHandler = "/data_persist/oem-aa-mod/aaw_handler";
const char *kDefaultConf    = "/data_persist/oem-aa-mod/libpatch.conf";
const char *kBcaIface       = "com.jci.bca";

const char *kDefaultReset   = "/data_persist/wl/reset-aa-dongle.sh";

const char *g_handler = kDefaultHandler;
const char *g_conf    = kDefaultConf;
const char *g_reset   = kDefaultReset;
uint32_t    g_aaw_id  = 15;
const char *g_iface   = "wlan1";

// ---- AP recovery ---------------------------------------------------------
// A dongle can come up with a silent radio that the head unit cannot detect:
// hostapd starts, the driver reports the AP up, and no phone can hear it. The
// only reliable sign is the phone reporting WIFI_NETWORK_UNAVAILABLE (handler
// exit HANDLER_AP_UNREACHABLE). On that verdict the dongle is re-probed with
// reset-aa-dongle.sh. Re-probing at boot does not help: the chip's full
// initialization runs only when the interface is brought up.
//
// One verdict is enough by default. Each extra verdict costs the phone's full
// 36 s join timeout, while a reset takes ~4 s and the phone retries on its own.
int    g_unreachable_streak = 0;    // handlers in a row ending in HANDLER_AP_UNREACHABLE
int    g_resets_done        = 0;    // this boot
time_t g_last_reset         = 0;
int    g_reset_after        = 1;    // AAW_RESET_AFTER

// A phone leaving the car produces the same verdict: it completes the Bluetooth
// handshake and then never joins. Resetting then is pointless, can cost the
// phone its rejoin if it comes back during the ~5 s the AP is down, and uses up
// a reset. So once the phone has joined and connected on this AP instance, the
// radio is proven and three verdicts in a row are needed. A departure yields
// one verdict, two if Bluetooth reconnects, so three still catches a radio that
// fails later. A reset makes a new, unproven AP instance.
//
// So does the dongle losing power: the head unit cuts USB power in standby, and
// waking from standby without a reboot re-enumerates the dongle with a fresh
// radio that the phone may not be able to hear. The interface is then
// re-created and gets a new ifindex, so the proof is bound to the ifindex it
// was earned on.
bool      g_radio_proven     = false;
int       g_proven_ifindex   = -1;
const int kProvenResetAfter  = 3;

// A re-enumerated dongle is reset as soon as it is back, without waiting for a
// phone's verdict. g_ap_ifindex is the interface instance the AP was set up
// on: taken at startup and after every reset. A different ifindex means the
// dongle dropped off USB and came back. The rebind only helps once the new
// interface has been brought up, because the chip's full initialization runs
// then, so the reset waits until the interface is up (hostapd re-opens it on
// its own), at most kReenumUpWait seconds.
int          g_ap_ifindex     = -1;
time_t       g_reenum_seen    = 0;    // when the new instance was first seen, or 0
const time_t kReenumUpWait    = 10;

// Read an integer sysfs attribute of the AP interface; -1 if unreadable.
long iface_attr(const char *attr, int base)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/class/net/%s/%s", g_iface, attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char b[24];
    int n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = '\0';
    return strtol(b, nullptr, base);
}

// ifindex of the AP interface, or -1 if it does not exist.
int iface_ifindex(void) { return (int)iface_attr("ifindex", 10); }

// Is the AP interface administratively up (IFF_UP)?
bool iface_up(void)
{
    long flags = iface_attr("flags", 16);
    return flags >= 0 && (flags & 0x1);
}

// The phone's channel. A reset closes our end of the PTY without BCA noticing,
// and BCA does not announce the channel again for ~25 s. So if BCA has not
// reported the channel down, the phone is invited again on the same PTY as soon
// as the AP is back (aaw_handler handles an already-open channel).
char g_last_pty[256] = {0};
bool g_channel_up    = false;
// BCA's device id of the phone on the channel; passed to aaw_handler, which
// passes it to jciAAPA so the session is tied to the right paired phone.
uint32_t g_last_bt_dev = 0;

// Silent-radio window for aaw_handler (see aaw_handler.cpp). The same for every
// attempt, including right after a reset.
const char *kDeafWindow = "20";

void start_handler(const char *pty);   // used by reap_reset()
pid_t  g_reset_pid          = -1;   // running recovery script, or -1
time_t g_reset_started      = 0;
// A silent radio can need several resets before it works, and a verdict
// arrives only every ~42 s (36 s join timeout plus the Bluetooth reconnect).
// A reset can only follow a verdict, so the cooldown just guards against a
// misbehaving phone; it is shorter than one failed cycle (~10 s once the phone
// is invited again right after the reset). kMaxResets bounds the retries.
const int    kMaxResets     = 10;   // per boot
const time_t kResetCooldown = 5;
const time_t kResetHardStop = 150;  // s; a script running longer is stuck

pid_t g_child = -1;       // running aaw_handler, or -1
DBusConnection *g_svc_conn = nullptr;  // service bus, for WirelessAaDisconnected

// ---- persistent boot log ---------------------------------------------------
// Runtime logs are in /tmp and lost at reboot. A few lines per boot go to the
// persistent file the AP scripts also write, so it is possible to tell later
// whether a phone ever reached the head unit. Lines are keyed by the boot id
// because the clock restarts at 1970 on every boot. First occurrence only.
const char *kBootLog = "/data_persist/log/aaw-boot.log";
char g_boot_id[16] = "noid";

void read_boot_id(void)
{
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY);
    if (fd < 0) return;
    char b[64];
    int n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    if (n >= 8) { memcpy(g_boot_id, b, 8); g_boot_id[8] = '\0'; }
}

void boot_note(const char *fmt, ...)
{
    char msg[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    FILE *f = fopen(kBootLog, "a");
    if (!f) return;
    fprintf(f, "id=%s stage=listend %s\n", g_boot_id, msg);
    fclose(f);
}

void log_ts(const char *fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%ld aaw_listend: %s\n", (long)time(nullptr), line);
    fflush(stdout);
}

// AP interface: AAW_IFACE, else aa_iface from libpatch.conf (last one wins, as
// in the shim), else wlan1.
void resolve_iface(void)
{
    if (const char *e = getenv("AAW_IFACE")) { if (*e) { g_iface = e; return; } }
    FILE *f = fopen(g_conf, "r");
    if (!f) return;
    static char found[32];
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "aa_iface", 8) != 0) continue;
        p = strchr(p, '=');
        if (!p) continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        int i = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '#' && *p != '\r' && *p != '\n'
               && i < (int)sizeof(found) - 1)
            found[i++] = *p++;
        found[i] = '\0';
        if (i > 0) g_iface = found;
    }
    fclose(f);
}

void reap(void)
{
    if (g_child <= 0) return;
    int st = 0;
    pid_t r = waitpid(g_child, &st, WNOHANG);
    if (r == 0) return;                                  // still running
    if (r < 0) {
        log_ts("aaw_handler pid=%d exited (waitpid: %s)", (int)g_child, strerror(errno));
        g_child = -1;
        return;
    }
    if (WIFEXITED(st)) {
        int code = WEXITSTATUS(st);
        log_ts("aaw_handler pid=%d exited (status %d)", (int)g_child, code);
        {
            static bool noted = false;
            if (!noted) { noted = true;
                boot_note("first-verdict=%s",
                          code == aaw::HANDLER_SESSION_OK     ? "session-ok" :
                          code == aaw::HANDLER_AP_UNREACHABLE ? "ap-unreachable" : "no-verdict"); }
        }
        if (code == aaw::HANDLER_AP_UNREACHABLE) {
            if (g_radio_proven) {
                int now_ifindex = iface_ifindex();
                if (now_ifindex != g_proven_ifindex) {
                    g_radio_proven = false;
                    log_ts("%s was re-created since the phone last joined (ifindex %d -> %d): "
                           "the dongle re-enumerated, so this AP is unproven",
                           g_iface, g_proven_ifindex, now_ifindex);
                }
            }
            ++g_unreachable_streak;
            log_ts("the phone could not find the AP (%d in a row, %d needed to reset)%s",
                   g_unreachable_streak,
                   g_radio_proven ? kProvenResetAfter : g_reset_after,
                   g_radio_proven ? "; this AP has carried a session, so the phone most "
                                    "likely left" : "");
        } else {
            if (code == aaw::HANDLER_SESSION_OK) {
                int now_ifindex = iface_ifindex();
                if (!g_radio_proven || now_ifindex != g_proven_ifindex) {
                    g_radio_proven = true;
                    g_proven_ifindex = now_ifindex;
                    log_ts("radio proven: the phone joined this AP on %s ifindex %d (%d "
                           "verdicts in a row now needed to reset)",
                           g_iface, now_ifindex, kProvenResetAfter);
                }
            }
            g_unreachable_streak = 0;
        }
    } else {
        // Stopped by stop_handler() or crashed: no verdict, streak unchanged.
        log_ts("aaw_handler pid=%d exited (signal %d)", (int)g_child,
               WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    g_child = -1;
}

void stop_handler(void)
{
    // The handler usually exits by itself when the phone drops the channel;
    // collect its exit status before signalling, or the verdict is lost.
    reap();
    if (g_child <= 0) return;
    log_ts("stopping aaw_handler pid=%d", (int)g_child);
    kill(g_child, SIGTERM);
    for (int i = 0; i < 20 && g_child > 0; ++i) { usleep(50000); reap(); }
    if (g_child > 0) { kill(g_child, SIGKILL); reap(); }
}

bool reset_in_flight(void) { return g_reset_pid > 0; }

// The recovery script runs as a child process reaped from the main loop, not
// with system(): it can take over a minute, and the loop must keep handling
// BCA signals meanwhile.
void reap_reset(void)
{
    if (g_reset_pid <= 0) return;
    int st = 0;
    pid_t r = waitpid(g_reset_pid, &st, WNOHANG);
    if (r == 0) {
        // All waits in the script are bounded, so overrunning this cap means it
        // is stuck. SIGTERM first, SIGKILL on the next pass.
        if (g_reset_started && time(nullptr) - g_reset_started > kResetHardStop) {
            log_ts("ERROR: %s still running after %lds; terminating it", g_reset,
                   (long)kResetHardStop);
            kill(g_reset_pid, SIGTERM);
            g_reset_started = 0;
        } else if (!g_reset_started) {
            kill(g_reset_pid, SIGKILL);
        }
        return;
    }
    if (r < 0) {
        log_ts("recovery script pid=%d: waitpid: %s", (int)g_reset_pid, strerror(errno));
    } else if (WIFEXITED(st)) {
        int code = WEXITSTATUS(st);
        if (code == 0)
            log_ts("%s reports the AP back up", g_reset);
        else
            log_ts("ERROR: %s exited %d, the AP is down; see "
                   "/data_persist/log/aaw-dongle-reset.log", g_reset, code);
    } else {
        log_ts("ERROR: %s killed by signal %d, the AP is down", g_reset,
               WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    bool ap_back = (r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    // Cleared before inviting the phone: start_handler() refuses while a reset runs.
    g_reset_pid = -1;
    g_reset_started = 0;
    // The rebind re-created the interface: this is the AP's instance now.
    g_ap_ifindex  = iface_ifindex();
    g_reenum_seen = 0;

    if (ap_back && g_channel_up && g_child <= 0 && g_last_pty[0]) {
        log_ts("inviting the phone again on %s", g_last_pty);
        start_handler(g_last_pty);
    } else if (ap_back && !g_channel_up) {
        log_ts("AP is back; waiting for BCA to announce the phone's channel");
    }
}

// Run the recovery script unless the per-boot cap or the cooldown holds it
// back. Returns true if a reset started. `why` names the trigger in the log.
bool start_reset(const char *why)
{
    if (g_resets_done >= kMaxResets) {
        static int logged_at = -1;         // log once, not per poll
        if (logged_at != g_resets_done) {
            logged_at = g_resets_done;
            log_ts("%s, but %d dongle resets already ran this boot; not resetting again. "
                   "Replug the dongle or restart the unit.", why, g_resets_done);
        }
        return false;
    }
    time_t now = time(nullptr);
    if (g_last_reset && now - g_last_reset < kResetCooldown) {
        static time_t logged_for = 0;      // log once per cooldown, not per poll
        if (logged_for != g_last_reset) {
            logged_for = g_last_reset;
            log_ts("%s, but a reset ran %lds ago (cooldown %lds); waiting",
                   why, (long)(now - g_last_reset), (long)kResetCooldown);
        }
        return false;
    }
    if (access(g_reset, X_OK) != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            log_ts("%s, but %s is missing or not executable (%s); recovery disabled",
                   why, g_reset, strerror(errno));
        }
        return false;
    }

    g_unreachable_streak = 0;
    g_radio_proven = false;                // the reset makes a new AP instance
    g_last_reset = now;
    ++g_resets_done;
    pid_t pid = fork();
    if (pid < 0) { log_ts("ERR: fork for %s: %s", g_reset, strerror(errno)); return false; }
    if (pid == 0) {
        // stdout/stderr are inherited, so the script's output joins this log.
        execl(g_reset, g_reset, (char *)nullptr);
        fprintf(stderr, "aaw_listend: execl(%s) failed: %s\n", g_reset, strerror(errno));
        _exit(127);
    }
    g_reset_pid     = pid;
    g_reset_started = now;
    log_ts("resetting the dongle (%s): %s pid=%d (reset %d of %d this boot)",
           why, g_reset, (int)pid, g_resets_done, kMaxResets);
    return true;
}

// Re-probe the dongle once enough verdicts in a row say the phone cannot find
// the AP. The streak is kept until a reset actually starts, so the log shows
// the real count while the cap or cooldown holds a reset back.
void maybe_recover_ap(void)
{
    if (g_unreachable_streak < (g_radio_proven ? kProvenResetAfter : g_reset_after)) return;
    if (g_child > 0) return;               // a handshake is running
    if (reset_in_flight()) return;
    start_reset("the phone could not find the AP");
}

// Reset a dongle that re-enumerated (see g_ap_ifindex).
void watch_dongle(void)
{
    if (reset_in_flight()) return;
    int now_ifindex = iface_ifindex();
    if (now_ifindex < 0) return;           // dongle absent, e.g. USB power cut in standby
    if (g_ap_ifindex < 0) { g_ap_ifindex = now_ifindex; return; }
    if (now_ifindex == g_ap_ifindex) return;

    time_t now = time(nullptr);
    if (!g_reenum_seen) {
        g_reenum_seen = now;
        g_radio_proven = false;            // a new radio instance
        log_ts("%s was re-created (ifindex %d -> %d): the dongle re-enumerated",
               g_iface, g_ap_ifindex, now_ifindex);
    }
    // A handshake already running decides by itself: a joined phone proves the
    // new instance, and an unreachable verdict resets it (one is enough now).
    // Resetting under it would cut off a phone that may be joining.
    if (g_child > 0) {
        log_ts("not resetting the re-enumerated dongle: a handshake is running; "
               "its verdict decides");
        g_ap_ifindex  = now_ifindex;
        g_reenum_seen = 0;
        return;
    }
    bool up = iface_up();
    if (!up && now - g_reenum_seen < kReenumUpWait) return;
    if (start_reset(up ? "the dongle re-enumerated"
                       : "the dongle re-enumerated and its interface stayed down")) {
        g_ap_ifindex  = now_ifindex;       // replaced in reap_reset() once the rebind is done
        g_reenum_seen = 0;
    }
}

// Tell jciAAPA the phone is gone so it ends the session right away instead of
// waiting ~40 s for the dead TCP connection (see aaw_session.cpp). Sent on the
// service bus, no arguments.
void emit_wireless_disconnected(void)
{
    if (!g_svc_conn) { log_ts("WARN: no service bus; cannot emit WirelessAaDisconnected"); return; }
    DBusMessage *sig = dbus_message_new_signal("/com/jci/aaw", "com.jci.aaw", "WirelessAaDisconnected");
    if (!sig) return;
    dbus_connection_send(g_svc_conn, sig, nullptr);
    dbus_connection_flush(g_svc_conn);
    dbus_message_unref(sig);
    log_ts("emitted com.jci.aaw.WirelessAaDisconnected");
}

// Is a phone in Android Open Accessory mode (wired Android Auto)? Any brand
// re-enumerates as 18d1:2d00..2d05 in that mode; a phone that is only charging
// keeps its own ids. Unreadable /sys counts as "no".
bool usb_aoa_present(void)
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

void start_handler(const char *pty)
{
    // Never invite a phone onto Wi-Fi while it is in wired Android Auto mode.
    // Plugging in the cable during a wireless session drops the AAW channel,
    // and BCA reports it connected again about a second later. Running the
    // handshake then sends the phone back onto Wi-Fi before the USB transport
    // has even opened; the phone serves the Wi-Fi socket instead of USB, every
    // USB write times out, and the wired session never carries data.
    if (usb_aoa_present()) {
        log_ts("handshake skipped: a phone is in wired AA mode (18d1:2d0x)");
        return;
    }
    // While the dongle is being reset the AP is down, so a handshake would only
    // fail and add a false verdict. The phone retries once the AP is back.
    if (reset_in_flight()) {
        log_ts("handshake skipped: the dongle is being reset (%s pid=%d)",
               g_reset, (int)g_reset_pid);
        return;
    }
    stop_handler();             // one phone at a time
    pid_t pid = fork();
    if (pid < 0) { log_ts("ERR: fork: %s", strerror(errno)); return; }
    char bt_dev[16];
    snprintf(bt_dev, sizeof bt_dev, "%u", (unsigned)g_last_bt_dev);
    if (pid == 0) {
        setenv("AAW_DEAF_WINDOW", kDeafWindow, 1);
        execl(g_handler, g_handler, pty, g_conf, bt_dev, (char *)nullptr);
        fprintf(stderr, "aaw_listend: execl(%s) failed: %s\n", g_handler, strerror(errno));
        _exit(127);
    }
    g_child = pid;
    log_ts("started aaw_handler pid=%d on %s btDev=%s", (int)pid, pty, bt_dev);
}

// Read an (ay) terminalPath at the iterator into out; a bare ay works too.
void read_terminal_path(DBusMessageIter *it, char *out, size_t cap)
{
    out[0] = '\0';
    DBusMessageIter inner = *it;
    int t = dbus_message_iter_get_arg_type(&inner);
    if (t == DBUS_TYPE_STRUCT) {
        DBusMessageIter s; dbus_message_iter_recurse(&inner, &s); inner = s;
        t = dbus_message_iter_get_arg_type(&inner);
    }
    if (t != DBUS_TYPE_ARRAY) return;
    if (dbus_message_iter_get_element_type(&inner) != DBUS_TYPE_BYTE) return;
    DBusMessageIter arr; dbus_message_iter_recurse(&inner, &arr);
    const unsigned char *bytes = nullptr; int n = 0;
    dbus_message_iter_get_fixed_array(&arr, &bytes, &n);
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    memcpy(out, bytes, (size_t)n);
    out[n] = '\0';
}

bool get_u32(DBusMessageIter *it, uint32_t *v)
{
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_UINT32) return false;
    dbus_uint32_t tmp; dbus_message_iter_get_basic(it, &tmp); *v = (uint32_t)tmp;
    return true;
}

void handle_signal(DBusMessage *msg)
{
    const char *member = dbus_message_get_member(msg);
    if (!member) return;
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) return;

    char pty[256] = {0};
    bool relevant = false;
    uint32_t btDev = 0;

    if (strcmp(member, "WcpConnectionStatusResp") == 0) {
        // (connStatus u, btDeviceId u, terminalPath (ay))
        uint32_t connStatus = 0;
        if (!get_u32(&it, &connStatus)) return; dbus_message_iter_next(&it);
        if (!get_u32(&it, &btDev))      return; dbus_message_iter_next(&it);
        read_terminal_path(&it, pty, sizeof pty);
        log_ts("WcpConnectionStatusResp: connStatus=%u btDev=%u terminalPath=\"%s\"",
               connStatus, btDev, pty);
        relevant = true;
    } else if (strcmp(member, "ConnectionStatusResp") == 0) {
        // (serviceId u, connStatus u, btDeviceId u, status u, terminalPath (ay))
        uint32_t serviceId = 0, connStatus = 0, status = 0;
        if (!get_u32(&it, &serviceId)) return; dbus_message_iter_next(&it);
        if (!get_u32(&it, &connStatus)) return; dbus_message_iter_next(&it);
        if (!get_u32(&it, &btDev))      return; dbus_message_iter_next(&it);
        if (!get_u32(&it, &status))     return; dbus_message_iter_next(&it);
        if (serviceId != g_aaw_id) return;
        read_terminal_path(&it, pty, sizeof pty);
        log_ts("ConnectionStatusResp AAW: connStatus=%u btDev=%u status=%u terminalPath=\"%s\"",
               connStatus, btDev, status, pty);
        relevant = true;
    }

    if (!relevant) return;
    if (pty[0]) {
        {
            static bool noted = false;    // a phone reached the head unit at all
            if (!noted) { noted = true; boot_note("bt-aaw-channel-up pty=%s", pty); }
        }
        g_channel_up = true;
        snprintf(g_last_pty, sizeof g_last_pty, "%s", pty);
        g_last_bt_dev = btDev;
        start_handler(pty);
    } else {
        g_channel_up = false;
        emit_wireless_disconnected();
        stop_handler();
    }
}

// Open a bus, register, and match all com.jci.bca signals. Returns null on failure.
DBusConnection *bus_open(const char *addr)
{
    DBusError err; dbus_error_init(&err);
    DBusConnection *conn = dbus_connection_open(addr, &err);
    if (!conn) { dbus_error_free(&err); return nullptr; }
    if (!dbus_bus_register(conn, &err)) {
        log_ts("bus_register(%s) failed: %s", addr, dbus_error_is_set(&err) ? err.message : "?");
        dbus_error_free(&err); dbus_connection_unref(conn); return nullptr;
    }
    char rule[96];
    snprintf(rule, sizeof rule, "type='signal',interface='%s'", kBcaIface);
    dbus_bus_add_match(conn, rule, &err);
    dbus_connection_flush(conn);
    if (dbus_error_is_set(&err)) {
        log_ts("add_match(%s) failed: %s", addr, err.message);
        dbus_error_free(&err); dbus_connection_unref(conn); return nullptr;
    }
    log_ts("subscribed to %s.* on %s", kBcaIface, addr);
    return conn;
}

// Candidate addresses: two environment overrides plus socket and TCP.
struct Bus { const char *cands[4]; int nc; DBusConnection *conn; };

DBusConnection *bus_try(Bus *b)
{
    for (int i = 0; i < b->nc; ++i) {
        DBusConnection *c = bus_open(b->cands[i]);
        if (c) return c;
    }
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1) g_conf = argv[1];
    if (const char *h = getenv("AAW_HANDLER"))    g_handler = h;
    if (const char *i = getenv("AAW_SERVICE_ID")) g_aaw_id  = (uint32_t)strtoul(i, nullptr, 10);
    if (const char *r = getenv("AAW_RESET"))       g_reset   = r;
    resolve_iface();
    if (const char *n = getenv("AAW_RESET_AFTER")) {
        int v = (int)strtol(n, nullptr, 10);
        if (v > 0) g_reset_after = v;
    }

    // BCA's status signals are on the HMI bus; the service bus is watched too.
    // Address order: our override, the OEM's environment variable, socket, TCP.
    const char *e;
    Bus hmi = {{nullptr,nullptr,nullptr,nullptr}, 0, nullptr};
    e = getenv("AAW_HMI_BUS");     if (e && *e) hmi.cands[hmi.nc++] = e;
    e = getenv("JCI_HMI_BUS");     if (e && *e && strcmp(e, ":unavailable")) hmi.cands[hmi.nc++] = e;
    hmi.cands[hmi.nc++] = kHmiSock;
    hmi.cands[hmi.nc++] = kHmiTcp;

    Bus svc = {{nullptr,nullptr,nullptr,nullptr}, 0, nullptr};
    e = getenv("AAW_BUS_ADDRESS"); if (e && *e) svc.cands[svc.nc++] = e;
    e = getenv("JCI_SERVICE_BUS"); if (e && *e && strcmp(e, ":unavailable")) svc.cands[svc.nc++] = e;
    svc.cands[svc.nc++] = kServiceSock;
    svc.cands[svc.nc++] = kServiceTcp;

    Bus *buses[2] = { &hmi, &svc };

    signal(SIGCHLD, SIG_DFL);   // children are reaped with WNOHANG
    signal(SIGPIPE, SIG_IGN);

    log_ts("starting (conf=%s, handler=%s, serviceId=%u, iface=%s, reset after %d)",
           g_conf, g_handler, g_aaw_id, g_iface, g_reset_after);
    log_ts("recovery script: %s (%s)", g_reset,
           access(g_reset, X_OK) == 0 ? "present" : "missing, recovery disabled");
    g_ap_ifindex = iface_ifindex();
    log_ts("AP interface %s ifindex %d", g_iface, g_ap_ifindex);
    read_boot_id();
    boot_note("started serviceId=%u iface=%s recovery=%s", g_aaw_id, g_iface,
              access(g_reset, X_OK) == 0 ? "armed" : "MISSING");

    for (;;) {
        bool any = false;
        for (int i = 0; i < 2; ++i) {
            Bus *b = buses[i];
            if (!b->conn) b->conn = bus_try(b);
            if (i == 1) g_svc_conn = b->conn;
            if (!b->conn) continue;
            any = true;
            dbus_connection_read_write(b->conn, 120);   // up to 120 ms per bus
            DBusMessage *msg;
            while ((msg = dbus_connection_pop_message(b->conn)) != nullptr) {
                if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
                    handle_signal(msg);
                dbus_message_unref(msg);
            }
            if (!dbus_connection_get_is_connected(b->conn)) {
                log_ts("bus %d disconnected; reconnecting", i);
                dbus_connection_unref(b->conn); b->conn = nullptr;
                if (i == 1) g_svc_conn = nullptr;
            }
        }
        reap();
        reap_reset();
        maybe_recover_ap();
        watch_dongle();
        if (!any) sleep(2);     // no bus yet
    }
    return 0;
}
