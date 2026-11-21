#!/bin/sh
# start-aa-ap.sh — start the 5 GHz AP on wlan1 for wireless Android Auto,
# once at boot, and keep it up for the whole drive.
#
# hostapd runs from /tmp/aa-hostapd-wlan1.conf, which libpatch-aap_service
# writes from libpatch.conf when aap_service loads, so the AP matches what the
# phone is told over Bluetooth. Run as a one-shot sm service that depends on
# the Wi-Fi driver service (load_wlan1) and on aap_service; see
# sm.conf.snippet. The wait for the file below covers running it by hand.
#
# NMS is not involved, so the built-in radio (wlan0) is untouched. The unit
# may run its own hostapd too (OEM Wi-Fi, wireless CarPlay), so hostapd is
# always matched by its config path in /proc/<pid>/cmdline, never by name.
#
# Built-in radio (aa_iface = wlan0, 2.4 GHz, no USB adapter): the interface is
# taken from aa_iface in libpatch.conf, so the same script serves both setups.
# On the built-in radio it does not wait for a USB hub, and it backs off if the
# unit's own Wi-Fi (wpa_supplicant, or a hostapd that is not ours) already uses
# the radio. libpatch-aap_service can start it itself (aa_ap_script), so no
# sm.conf service is needed; it is then also restarted with aap_service, which
# is why every step below is idempotent.

HERE=$(dirname "$(readlink -f "$0")")
# libpatch.conf and the helpers: next to this script (MZD Tweaks layout), else
# the classic /data_persist/oem-aa-mod. AA_CONF_LP / AAW_DIR override both.
if [ -n "${AAW_DIR:-}" ]; then TOOLS_DIR=$AAW_DIR
elif [ -f "$HERE/aaw_listend" ]; then TOOLS_DIR=$HERE
else TOOLS_DIR=/data_persist/oem-aa-mod; fi
# wireless.conf (package-owned AP keys, read by the shims before libpatch.conf)
# is what the helpers get when present: aaw_handler needs only the aa_* keys.
if [ -n "${AA_CONF_LP:-}" ]; then CONF_LP=$AA_CONF_LP
elif [ -f "$HERE/wireless.conf" ]; then CONF_LP=$HERE/wireless.conf
elif [ -f "$HERE/libpatch.conf" ]; then CONF_LP=$HERE/libpatch.conf
else CONF_LP=/data_persist/oem-aa-mod/libpatch.conf; fi
# aa_iface: AA_IFACE, else the last aa_iface line (wireless.conf, then
# libpatch.conf, so a user override wins, as in the shims), else wlan1.
IFACE=${AA_IFACE:-$(cat "$HERE/wireless.conf" "$CONF_LP" "$HERE/libpatch.conf" 2>/dev/null | sed -n 's/^[[:space:]]*aa_iface[[:space:]]*=[[:space:]]*\([A-Za-z0-9_.-]*\).*/\1/p' | tail -n 1)}
[ -n "$IFACE" ] || IFACE=wlan1
AP_IP=192.168.53.1
CONF=/tmp/aa-hostapd-wlan1.conf
DHCP=/tmp/aa-udhcpd.conf
LEASES=/tmp/aa-udhcpd.leases     # udhcpd's default /var/lib/misc/ does not exist here
log() { echo "start-aa-ap: $*"; }


# ---- per-boot breadcrumb -----------------------------------------------------
# Runtime logs are in /tmp and lost at reboot, so one line per boot goes to a
# persistent file. Lines are keyed by the first 8 characters of
# /proc/sys/kernel/random/boot_id, because the clock restarts at 1970 on every
# boot. reset-aa-dongle.sh and aaw_listend use the same key. A driver loader may
# write its own line to the same file; if that line is present and this one is
# missing, aap_service never came up.
BOOTLOG=${BOOTLOG:-/data_persist/log/aaw-boot.log}
BOOT_ID=$(cut -c1-8 /proc/sys/kernel/random/boot_id 2>/dev/null)
[ -z "$BOOT_ID" ] && BOOT_ID=noid

# State read from /sys. The full USB device list tells "hub down", "dongle
# absent" and "present but not bound" apart without assuming a dongle model.
bc_common() {
    bc_up=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    bc_usb=$(ls /sys/bus/usb/devices 2>/dev/null | tr '\n' ',')
    bc_mod=no
    for m in rtl8812au rt2800usb; do
        grep -q "^$m " /proc/modules 2>/dev/null && bc_mod=$m
    done
    bc_bound=no
    if [ "$bc_mod" != no ]; then
        set -- /sys/bus/usb/drivers/$bc_mod/*-*:*
        [ -e "$1" ] && bc_bound=${1##*/}
    fi
    bc_wlans=$(ls -d /sys/class/net/wlan* 2>/dev/null | sed 's|.*/||' | tr '\n' ',')
    [ -z "$bc_wlans" ] && bc_wlans=NONE
}

# On a failing boot, keep the relevant dmesg lines (bounded, keyed to the boot).
bc_dmesg() {
    dmesg 2>/dev/null | grep -iE 'usb|hub|8812|rtl87|rt2800|wlan' | tail -40 \
        >> "/data_persist/log/aaw-boot-$BOOT_ID.dmesg" 2>/dev/null
}

# Is the external USB hub powered and enumerated? Every USB device sits behind
# it, so this separates "unit still in standby" from "dongle missing". Root hubs
# (N-0:1.0) and host controllers (usbN) are always present; anything else is a
# device behind the hub.
hub_present() {
    for d in /sys/bus/usb/devices/*; do
        case "${d##*/}" in
            usb[0-9]*|[0-9]-0:1.0|'*') continue ;;
            *) return 0 ;;
        esac
    done
    return 1
}

RESULT=started-unexpected-exit    # updated as the script progresses
WAIT_CONF=0; WAIT_IFACE=0; HOSTAPD_TRIES=0; HOSTAPD_ERR=-; HUB_AT=never

breadcrumb() {
    mkdir -p "$(dirname "$BOOTLOG")" 2>/dev/null
    bc_common
    hp=no; for pid in $(pidof hostapd 2>/dev/null); do
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "$CONF" && hp=yes
    done
    dp=no; for pid in $(pidof udhcpd 2>/dev/null); do
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "$DHCP" && dp=yes
    done
    lp=no; pidof aaw_listend >/dev/null 2>&1 && lp=yes
    mac=-; [ -e "/sys/class/net/$IFACE/address" ] && mac=$(cat "/sys/class/net/$IFACE/address")
    echo "id=$BOOT_ID stage=ap up=${bc_up:-?}s iface=$IFACE:$( [ -e /sys/class/net/$IFACE ] \
         && echo present || echo MISSING ) mod=$bc_mod bound=$bc_bound wlans=$bc_wlans" \
         "usb=$bc_usb mac=$mac conf=$( [ -f "$CONF" ] && echo yes || echo NO )" \
         "waitconf=${WAIT_CONF}s waitiface=${WAIT_IFACE}s hubat=$HUB_AT hostapd=$hp tries=$HOSTAPD_TRIES" \
         "hostapderr=$HOSTAPD_ERR udhcpd=$dp listend=$lp result=$RESULT" >> "$BOOTLOG" 2>/dev/null
    [ "$RESULT" = ap-up ] || bc_dmesg
}
trap breadcrumb EXIT
log "boot id $BOOT_ID (breadcrumb -> $BOOTLOG)"

# Is our hostapd (the one serving $CONF) running?
our_hostapd_running() {
    for pid in $(pidof hostapd 2>/dev/null); do
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "$CONF" && return 0
    done
    return 1
}

# Already up: nothing to do (safe on an sm retry).
if our_hostapd_running; then
    log "our hostapd ($CONF) already running — nothing to do"
    RESULT=already-running
    exit 0
fi

# Wait up to 30 s for libpatch-aap_service to write the conf.
i=0
while [ ! -f "$CONF" ] && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
WAIT_CONF=$i
if [ ! -f "$CONF" ]; then
    RESULT=no-ap-conf
    log "ERROR: $CONF not present after ${i}s — is wireless=true and is"
    log "       libpatch-aap_service.so preloaded into aap_service? Aborting AP."
    exit 0
fi

# Wait for the interface to appear. The driver service exits as soon as the
# module is loaded, and the interface appears a moment later; starting hostapd
# before that fails.
#
# The wait has to be long. When the unit wakes without ignition (door, CAN), it
# powers the external USB hub down, so no USB device exists until ignition
# arrives, which can be several minutes after boot. This is a one-shot service:
# giving up early means no wireless Android Auto for the rest of the drive.
#
# While the hub is down, keep waiting (up to 15 minutes, so a unit with no
# dongle does not wait forever). Once the hub is up the dongle appears within a
# second or two, so allow 30 s from that point. Progress is logged once a
# minute. Nothing else depends on this service, so nothing is blocked.
# Built-in radio: on the SoC bus (SDIO), not behind the USB hub. It exists from
# boot, so wait briefly and never for the hub. Leave it alone if the unit's own
# Wi-Fi is using it.
case "$(readlink -f "/sys/class/net/$IFACE/device" 2>/dev/null)" in
    */usb*|'') BUILTIN=0 ;;
    *) BUILTIN=1 ;;
esac
[ "$IFACE" = wlan0 ] && BUILTIN=1
if [ "$BUILTIN" = 1 ]; then
    i=0
    while [ ! -e "/sys/class/net/$IFACE" ] && [ "$i" -lt 60 ]; do sleep 1; i=$((i + 1)); done
    WAIT_IFACE=$i
    if [ ! -e "/sys/class/net/$IFACE" ]; then
        RESULT=NO-IFACE-builtin
        log "ERROR: built-in $IFACE did not appear in ${i}s — AP not started"
        exit 0
    fi
    if pidof wpa_supplicant >/dev/null 2>&1; then
        RESULT=builtin-busy-wpa_supplicant
        log "built-in $IFACE is in use by the unit's Wi-Fi (wpa_supplicant) — leaving it alone"
        exit 0
    fi
    for pid in $(pidof hostapd 2>/dev/null); do
        if ! tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "$CONF"; then
            RESULT=builtin-busy-hostapd
            log "built-in $IFACE is in use by another hostapd (pid $pid) — leaving it alone"
            exit 0
        fi
    done
    log "built-in radio $IFACE (2.4 GHz unless aa_channel > 14) — no USB wait"
fi
i=0
while [ "$BUILTIN" = 0 ] && [ ! -e "/sys/class/net/$IFACE" ] && [ "$i" -lt 900 ]; do
    sleep 1; i=$((i + 1))
    if hub_present; then
        if [ "$HUB_AT" = never ]; then
            HUB_AT=${i}s
            log "USB hub tree enumerated after ${i}s — now waiting for $IFACE"
        fi
        # 30 s from the moment the hub appeared.
        [ "$((i - ${HUB_AT%s}))" -ge 30 ] && break
    elif [ $((i % 60)) -eq 0 ]; then
        log "no USB hub yet (${i}s) — powered down until ignition; holding the AP wait open"
    fi
done
[ "$BUILTIN" = 1 ] || WAIT_IFACE=$i
if [ ! -e "/sys/class/net/$IFACE" ]; then
    RESULT=NO-IFACE-dongle-absent
    if [ "$HUB_AT" = never ]; then
        RESULT=NO-USB-HUB-still-in-standby
        log "ERROR: the external USB hub never powered up in ${i}s — the unit stayed in"
        log "       standby (woken without ignition), so nothing could be on USB. Not a"
        log "       dongle fault; SYSTEM re-energises the hub when ignition arrives."
    else
        log "ERROR: hub came up at $HUB_AT but $IFACE never appeared (${i}s total) — the"
        log "       dongle is absent or its probe failed. See the breadcrumb dmesg dump."
    fi
    exit 0
fi
sleep 1                                   # let the new interface settle

# Bring the interface up with the AP address.
ifconfig "$IFACE" up 2>/dev/null
ifconfig "$IFACE" "$AP_IP" netmask 255.255.255.0 2>/dev/null

# The INPUT chain's policy is DROP and only wlan0 is allowed, so the phone's
# DHCP request and its connection to the AA TCP server would be dropped on
# wlan1. Accept everything on the AP interface. -C avoids duplicate rules on a
# re-run.
iptables -C INPUT -i "$IFACE" -j ACCEPT 2>/dev/null || iptables -I INPUT -i "$IFACE" -j ACCEPT
log "firewall: INPUT accept on $IFACE ensured"

# Start hostapd, with retries: the new interface can be briefly busy.
started=0; t=0
while [ "$t" -lt 5 ]; do
    HOSTAPD_ERR=$(hostapd -B "$CONF" 2>&1 >/dev/null | tail -1 | tr ' ' '_')
    [ -z "$HOSTAPD_ERR" ] && HOSTAPD_ERR=-
    if our_hostapd_running; then started=1; break; fi
    t=$((t + 1)); HOSTAPD_TRIES=$t
    log "hostapd attempt $t failed ($HOSTAPD_ERR); retrying in 2s..."
    sleep 2; ifconfig "$IFACE" up 2>/dev/null
done
if [ "$started" = 1 ]; then
    log "hostapd up on $IFACE ($AP_IP) from $CONF (after $t retr$( [ "$t" = 1 ] && echo y || echo ies ))"
else
    RESULT=hostapd-failed
    log "ERROR: hostapd failed to start on $CONF after $t attempts"
    exit 0
fi

# DHCP for the phone. The AA TCP server listens on $AP_IP:<aa_tcp_port>.
printf 'start 192.168.53.20\nend 192.168.53.254\ninterface %s\noption subnet 255.255.255.0\nopt router %s\nopt lease 86400\nlease_file %s\n' \
    "$IFACE" "$AP_IP" "$LEASES" > "$DHCP"
mkdir -p /var/lib/misc 2>/dev/null        # some udhcpd builds use this path regardless
: > "$LEASES" 2>/dev/null
if udhcpd "$DHCP" 2>/dev/null; then
    log "udhcpd up on $IFACE (leases in $LEASES)"
else
    log "WARNING: udhcpd failed — phone may associate but get no IP"
fi

# aaw_listend waits for the phone's wireless AA Bluetooth connection and runs
# aaw_handler for it. Skipped if not installed; the AP still comes up.
LISTEND=$TOOLS_DIR/aaw_listend
HANDLER=$TOOLS_DIR/aaw_handler
# Installers that cannot set the executable bit (MZD Tweaks installs files 0644)
# get runnable copies in RAM; /tmp is not mounted noexec on the unit.
if [ -f "$LISTEND" ] && [ -f "$HANDLER" ] && { [ ! -x "$LISTEND" ] || [ ! -x "$HANDLER" ]; }; then
    mkdir -p /tmp/aaw && cp "$LISTEND" "$HANDLER" /tmp/aaw/ && chmod 755 /tmp/aaw/aaw_listend /tmp/aaw/aaw_handler \
        && LISTEND=/tmp/aaw/aaw_listend && HANDLER=/tmp/aaw/aaw_handler
fi
# The dongle re-probe script unbinds a USB device; it has no meaning for the
# built-in radio, so recovery stays off there.
[ "$BUILTIN" = 1 ] && AAW_RESET=${AAW_RESET:-/nonexistent-builtin-radio} && export AAW_RESET
if [ -x "$LISTEND" ] && [ -x "$HANDLER" ]; then
    if pidof aaw_listend >/dev/null 2>&1; then
        log "aaw_listend already running"
    else
        AAW_HANDLER="$HANDLER" AAW_IFACE="$IFACE" "$LISTEND" "$CONF_LP" >/tmp/aaw_listend.log 2>&1 &
        log "aaw_listend started (pid $!) — log: /tmp/aaw_listend.log"
    fi
else
    log "aaw_listend/aaw_handler not in $TOOLS_DIR — skipping auto-spawn"
fi
RESULT=ap-up
exit 0
