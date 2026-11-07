// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libpatch-aap_service.so — wireless Android Auto: AP credentials, AA TCP
// server, and the AP's hostapd.conf.
//
// This runs the head unit's own native wireless Android Auto over real Wi-Fi;
// nothing is emulated.
//
// 1. AP credentials. aap_service initializes wireless projection with
//      CCWifiProjection::cc_wifi_init(bssid, ssid, pwd, security, nchan, channels)
//    aap_service passes them all empty, so the function's argument check
//    fails, cc_init logs "Wifi projection init failed", and the phone is never
//    offered wireless. A detour at the
//    function's entry replaces the arguments with the aa_* values from
//    libpatch.conf. The BSSID is aa_bssid, or the live MAC of aa_iface.
//
// 2. AA TCP server. The AAPTransportIface constructor starts the TCP listener
//    only if its AAP_TransInfo has a valid IPv4 address and a non-zero port;
//    the OEM leaves both empty ("Not instantiating TCP server"). A detour fills
//    in aa_tcp_ip and aa_tcp_port. The constructor's address check rejects
//    0.0.0.0, so for 0.0.0.0 that one call site is patched to report valid.
//    0.0.0.0 binds all interfaces, so there is no race with wlan1 getting its
//    address, and the phone can still connect to the AP address.
//
// 3. hostapd.conf. Written once at load from the same keys, so the AP on the
//    air is exactly what the phone is told. start-aa-ap.sh starts hostapd
//    with it at boot.
//
// aap_service is a non-PIE executable, so both functions are at fixed
// addresses. Each detour overwrites the first two instructions with
// `ldr pc,[pc,#-4]; .word detour`, replays them, and jumps back. The original
// words are checked before anything is written, and nothing is patched on a
// mismatch: aap_service is reset_board="yes", so a bad write would reboot the
// unit. Only active with wireless=true, and only in the aap_service process.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // dladdr() via common/config.h
#endif

#include "log.h"
#include "common/config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#undef  LOG_TAG
#define LOG_TAG "WIFIAP"

namespace {

// CCWifiProjection::cc_wifi_init: entry, the two words replaced, resume point.
constexpr uintptr_t kCcWifiInit   = 0x0003ec2c;
constexpr uint32_t  kPrologue0    = 0xe92d4ff0;   // push {r4-fp,lr}
constexpr uint32_t  kPrologue1    = 0xe59f45cc;   // ldr r4,[pc,#1484]  (r4 = *0x3f204)
constexpr uint32_t  kLdrPcRel     = 0xe51ff004;   // ldr pc,[pc,#-4]

constexpr const char *kOutPath = "/tmp/aa-hostapd-wlan1.conf";

// AAPTransportIface constructor: entry and the two words replaced. Neither is
// PC-relative, so both are replayed as-is.
constexpr uintptr_t kTransIfaceCtor   = 0x00030188;
constexpr uint32_t  kTPrologue0       = 0xe92d45f0;   // push {r4-r8,sl,lr}
constexpr uint32_t  kTPrologue1       = 0xe3a07000;   // mov  r7,#0
constexpr unsigned  kTransInfoPortOff = 0x80;         // AAP_TransInfo: char ip[] at +0, u32 port at +0x80

// The constructor's call to ales_osal_validate_ipv4, replaced by `mov r0,#0`
// (= valid) when aa_tcp_ip is 0.0.0.0. The function itself is untouched for
// every other caller.
constexpr uintptr_t kValidateCall     = 0x0003021c;
constexpr uint32_t  kValidateCallOrig = 0xeb00b6ea;   // bl ales_osal_validate_ipv4
constexpr uint32_t  kForceValidInsn   = 0xe3a00000;   // mov r0,#0

// Channel list passed to cc_wifi_init; set from aa_channel at load.
unsigned g_channels[1] = { 36 };

bool set_prot(uintptr_t addr, size_t len, int prot)
{
    const uintptr_t ps   = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t page = addr & ~(ps - 1);
    const size_t    span = (addr + len) - page;
    return mprotect(reinterpret_cast<void *>(page), span, prot) == 0;
}

bool in_aap_service()
{
    char buf[256];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';                       // argv[0]
    return strstr(buf, "aap_service") != nullptr;
}

// MAC of aa_iface as "xx:xx:xx:xx:xx:xx", cached after the first good read.
// Empty if the interface does not exist yet.
const char *iface_mac()
{
    static char mac[24] = "";
    if (mac[0]) return mac;
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address",
             libpatch_config::aa_iface());
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return mac;
    char buf[32];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return mac;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    snprintf(mac, sizeof(mac), "%s", buf);
    return mac;
}

} // namespace

// ---- hostapd.conf ------------------------------------------------------------
// Written once, via a temporary file and rename(), so a reader never sees a
// partial file. The content depends only on libpatch.conf (hostapd advertises
// the interface MAC itself), so later calls are no-ops.
// used: referenced only from inline asm.
extern "C" __attribute__((used)) void aa_write_hostapd_conf()
{
    static bool wrote_once = false;
    if (wrote_once) return;

    const char *ssid = libpatch_config::aa_ssid();
    const char *pwd  = libpatch_config::aa_psk();
    const char *ifc  = libpatch_config::aa_iface();
    unsigned    chan = libpatch_config::aa_channel();
    unsigned    sec  = libpatch_config::aa_security();
    const char *hw   = (chan <= 14) ? "g" : "a";

    char tmp[80];
    snprintf(tmp, sizeof tmp, "%s.tmp", kOutPath);
    FILE *f = std::fopen(tmp, "w");
    if (!f) { LOGE("cannot open %s for write", tmp); return; }

    std::fprintf(f, "# Generated by libpatch-aap_service from libpatch.conf (aa_* keys).\n");
    std::fprintf(f, "# Started at boot by start-aa-ap.sh.\n");

    std::fprintf(f, "interface=%s\n", ifc);
    std::fprintf(f, "driver=nl80211\n");
    std::fprintf(f, "ssid=%s\n", ssid);
    if (libpatch_config::aa_hidden())
        std::fprintf(f, "ignore_broadcast_ssid=1\n");   // the phone gets the SSID over Bluetooth
    std::fprintf(f, "hw_mode=%s\n", hw);
    std::fprintf(f, "channel=%u\n", chan);
    std::fprintf(f, "ieee80211d=0\n");           // the driver applies its own regulatory domain
    std::fprintf(f, "wmm_enabled=1\n");
    std::fprintf(f, "ieee80211n=1\n");
    if (chan > 14)
        std::fprintf(f, "ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40]\n");
    std::fprintf(f, "ctrl_interface=/var/run/hostapd\n");
    // AAP_WifiSecurityMode, as sent to the phone: 1 = OPEN, 5 = WPA2_PERSONAL.
    // OPEN has no link encryption; Android Auto's own TLS still protects the
    // projection, as it does over USB. Anything other than 1 means WPA2.
    if (sec == 1) {
        std::fprintf(f, "auth_algs=1\n");
    } else {
        std::fprintf(f, "wpa=2\n");
        std::fprintf(f, "wpa_passphrase=%s\n", pwd);
        std::fprintf(f, "wpa_key_mgmt=WPA-PSK\n");
        std::fprintf(f, "rsn_pairwise=CCMP\n");
    }
    std::fclose(f);
    if (std::rename(tmp, kOutPath) != 0) {
        LOGE("wifi-ap: rename %s -> %s failed", tmp, kOutPath);
        return;
    }
    wrote_once = true;
    LOGE("wifi-ap: wrote %s (iface=%s ssid=\"%s\" ch%u hw=%s security=%u)",
         kOutPath, ifc, ssid, chan, hw, sec);
}

// ---- cc_wifi_init arguments ------------------------------------------------
// Called from the detour. slots[] holds the six arguments:
//   [0]=bssid [1]=ssid [2]=pwd [3]=security [4]=nchan [5]=channels
// They are replaced in place; the detour writes them back to r1-r3 and the
// caller's stack.
extern "C" __attribute__((used)) void aa_wifi_init_filter(uint32_t *slots)
{
    const char *in_bssid = reinterpret_cast<const char *>(slots[0]);
    const char *in_ssid  = reinterpret_cast<const char *>(slots[1]);
    const char *in_pwd   = reinterpret_cast<const char *>(slots[2]);
    LOGE("wifi-ap: cc_wifi_init in: bssid=\"%s\" ssid=\"%s\" pwd=%s sec=%u nchan=%u",
         in_bssid ? in_bssid : "(null)", in_ssid ? in_ssid : "(null)",
         (in_pwd && in_pwd[0]) ? "<set>" : "(empty)", slots[3], slots[4]);

    const char *bssid = libpatch_config::aa_bssid();
    if (!bssid || !bssid[0]) bssid = iface_mac();

    slots[0] = (uint32_t)(uintptr_t)bssid;
    slots[1] = (uint32_t)(uintptr_t)libpatch_config::aa_ssid();
    slots[2] = (uint32_t)(uintptr_t)libpatch_config::aa_psk();
    slots[3] = libpatch_config::aa_security();
    slots[4] = 1;
    slots[5] = (uint32_t)(uintptr_t)g_channels;

    LOGE("wifi-ap: cc_wifi_init out: bssid=%s ssid=\"%s\" ch%u sec=%u",
         bssid[0] ? bssid : "(interface missing, empty)",
         libpatch_config::aa_ssid(), g_channels[0], slots[3]);

    aa_write_hostapd_conf();   // normally already written at load
}

// Detour at cc_wifi_init's entry. On entry the registers and stack are the
// function's own: r0=this r1=bssid r2=ssid r3=pwd, [sp]=security
// [sp+4]=nchan [sp+8]=channels, lr=caller, r4-r11 untouched.
//
// Stack (entry sp = S):
//   after push{r0-r3,ip,lr}, sp = S-24: this@0 bssid@4 ssid@8 pwd@12 ip@16
//     lr@20, caller's security@24 nchan@28 channels@32
//   after sub sp,#32,        sp = S-56: scratch@0..20, everything above +32
extern "C" __attribute__((used, naked)) void cc_wifi_init_detour(void)
{
    __asm__ volatile(
        "push   {r0, r1, r2, r3, ip, lr}             \n"
        "sub    sp, sp, #32                          \n" // scratch, keeps 8-byte alignment
        "ldr    r0, [sp, #36]                        \n" // bssid
        "str    r0, [sp, #0]                         \n"
        "ldr    r0, [sp, #40]                        \n" // ssid
        "str    r0, [sp, #4]                         \n"
        "ldr    r0, [sp, #44]                        \n" // pwd
        "str    r0, [sp, #8]                         \n"
        "ldr    r0, [sp, #56]                        \n" // security
        "str    r0, [sp, #12]                        \n"
        "ldr    r0, [sp, #60]                        \n" // nchan
        "str    r0, [sp, #16]                        \n"
        "ldr    r0, [sp, #64]                        \n" // channels
        "str    r0, [sp, #20]                        \n"
        "mov    r0, sp                               \n"
        "bl     aa_wifi_init_filter                  \n"
        "ldr    r0, [sp, #12]                        \n" // security -> caller's stack
        "str    r0, [sp, #56]                        \n"
        "ldr    r0, [sp, #16]                        \n" // nchan    -> caller's stack
        "str    r0, [sp, #60]                        \n"
        "ldr    r0, [sp, #20]                        \n" // channels -> caller's stack
        "str    r0, [sp, #64]                        \n"
        "ldr    r0, [sp, #0]                         \n" // bssid -> saved r1
        "str    r0, [sp, #36]                        \n"
        "ldr    r0, [sp, #4]                         \n" // ssid  -> saved r2
        "str    r0, [sp, #40]                        \n"
        "ldr    r0, [sp, #8]                         \n" // pwd   -> saved r3
        "str    r0, [sp, #44]                        \n"
        "add    sp, sp, #32                          \n"
        "pop    {r0, r1, r2, r3, ip, lr}             \n" // r1-r3 = new values
        "push   {r4, r5, r6, r7, r8, r9, sl, fp, lr} \n" // replayed instruction 1
        "movw   r4, #0xf204                          \n" // replayed instruction 2:
        "movt   r4, #0x0003                          \n" //   r4 = *0x3f204
        "ldr    r4, [r4]                             \n"
        "movw   ip, #0xec34                          \n" // continue at 0x3ec34
        "movt   ip, #0x0003                          \n"
        "bx     ip                                   \n"
    );
}

// ---- AAP_TransInfo ---------------------------------------------------------
// Called from the constructor detour with the AAP_TransInfo the OEM built
// (empty IP, port 0).
extern "C" __attribute__((used)) void aa_inject_transinfo(void *ti)
{
    if (!ti) return;
    char    *p    = static_cast<char *>(ti);
    const char *ip = libpatch_config::aa_tcp_ip();
    unsigned port  = libpatch_config::aa_tcp_port();

    unsigned old_port = 0;
    std::memcpy(&old_port, p + kTransInfoPortOff, sizeof(old_port));
    LOGE("trans-info: ip=\"%s\" port=%u -> ip=\"%s\" port=%u", p, old_port, ip, port);

    // The IP field is an inline char array; 16 bytes hold any dotted quad.
    // (Unqualified snprintf: this libstdc++ has no std::snprintf.)
    snprintf(p, 16, "%s", ip);
    std::memcpy(p + kTransInfoPortOff, &port, sizeof(port));
}

// Detour at the AAPTransportIface constructor. r0=this r1=AAP_TransInfo*
// r2/r3=callbacks.
extern "C" __attribute__((used, naked)) void aap_transinfo_detour(void)
{
    __asm__ volatile(
        "push   {r0, r1, r2, r3, ip, lr}            \n"
        "mov    r0, r1                              \n"
        "bl     aa_inject_transinfo                 \n"
        "pop    {r0, r1, r2, r3, ip, lr}            \n"
        "push   {r4, r5, r6, r7, r8, sl, lr}        \n" // replayed instruction 1
        "mov    r7, #0                              \n" // replayed instruction 2
        "movw   ip, #0x0190                         \n" // continue at 0x30190
        "movt   ip, #0x0003                         \n"
        "bx     ip                                  \n"
    );
}

namespace {

void install_wifi_conf_hook()
{
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(kCcWifiInit);
    if (p[0] != kPrologue0 || p[1] != kPrologue1) {
        LOGE("cc_wifi_init: found {0x%08x,0x%08x}, expected {0x%08x,0x%08x}; "
             "unsupported aap_service, not patching", p[0], p[1], kPrologue0, kPrologue1);
        return;
    }
    if (!set_prot(kCcWifiInit, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        LOGE("cc_wifi_init: mprotect failed, not patching");
        return;
    }
    p[0] = kLdrPcRel;
    p[1] = (uint32_t)(uintptr_t)&cc_wifi_init_detour;
    __builtin___clear_cache((char *)kCcWifiInit, (char *)(kCcWifiInit + 8));
    set_prot(kCcWifiInit, 8, PROT_READ | PROT_EXEC);
    LOGE("wifi-ap: cc_wifi_init hook installed");
}

void install_transinfo_hook()
{
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(kTransIfaceCtor);
    if (p[0] != kTPrologue0 || p[1] != kTPrologue1) {
        LOGE("AAPTransportIface: found {0x%08x,0x%08x}, expected {0x%08x,0x%08x}; "
             "unsupported aap_service, not patching", p[0], p[1], kTPrologue0, kTPrologue1);
        return;
    }
    if (!set_prot(kTransIfaceCtor, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        LOGE("AAPTransportIface: mprotect failed, not patching");
        return;
    }
    p[0] = kLdrPcRel;
    p[1] = (uint32_t)(uintptr_t)&aap_transinfo_detour;
    __builtin___clear_cache((char *)kTransIfaceCtor, (char *)(kTransIfaceCtor + 8));
    set_prot(kTransIfaceCtor, 8, PROT_READ | PROT_EXEC);
    LOGE("trans-info: hook installed (ip=%s port=%u)",
         libpatch_config::aa_tcp_ip(), libpatch_config::aa_tcp_port());

    if (strcmp(libpatch_config::aa_tcp_ip(), "0.0.0.0") != 0) {
        return;
    }
    volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(kValidateCall);
    if (*v != kValidateCallOrig) {
        LOGE("trans-info: address check is 0x%08x, expected 0x%08x; not patching "
             "(0.0.0.0 will be rejected, set a real aa_tcp_ip)", *v, kValidateCallOrig);
        return;
    }
    if (!set_prot(kValidateCall, 4, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        LOGE("trans-info: mprotect failed on the address check");
        return;
    }
    *v = kForceValidInsn;
    __builtin___clear_cache((char *)kValidateCall, (char *)(kValidateCall + 4));
    set_prot(kValidateCall, 4, PROT_READ | PROT_EXEC);
    LOGE("trans-info: 0.0.0.0 accepted");
}

__attribute__((constructor))
void wifi_ap_conf_init()
{
    if (!in_aap_service()) {
        return;
    }
    libpatch_config::load(reinterpret_cast<const void *>(&wifi_ap_conf_init));
    if (!libpatch_config::wireless_enabled()) {
        return;
    }
    g_channels[0] = libpatch_config::aa_channel();
    aa_write_hostapd_conf();
    install_wifi_conf_hook();
    install_transinfo_hook();
}

} // namespace
