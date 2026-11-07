#!/bin/sh
# reset-aa-dongle.sh — re-probe the wireless AA Wi-Fi dongle and restart the AP.
#
# A dongle can come up with a silent radio while everything below it reports
# success: the driver binds, hostapd starts and rekeys on schedule, and the AP
# is never on the air. The phone completes the Bluetooth handshake and, 36 s
# later, reports WIFI_NETWORK_UNAVAILABLE, every time. Unplugging and replugging
# the dongle fixes it.
#
# The head unit cannot detect this itself: it has no second 5 GHz radio, and the
# driver's status files report its software state, which looks healthy. So
# aaw_listend runs this script on the phone's verdict. It does what a replug
# does, a full re-probe of the chip (firmware download, efuse, PHY/RF setup),
# through a driver unbind/bind. That avoids USB connect/disconnect events, which
# the OEM USB manager reacts to (it authorises devices here).
#
# The driver is read from sysfs and the interface from aa_iface in
# libpatch.conf, so nothing here depends on a particular dongle.
#
# Usage:  reset-aa-dongle.sh [iface]        (root; iface defaults to aa_iface)
# Run by aaw_listend; safe to run by hand. Removes nothing. If the interface
# does not come back it reports the failure and exits non-zero.
#
# Install as /data_persist/wl/reset-aa-dongle.sh (mode 0755).

LIBPATCH_CONF=${LIBPATCH_CONF:-/data_persist/oem-aa-mod/libpatch.conf}
AP_START=${AP_START:-/data_persist/wl/start-aa-ap.sh}
DIAG=${DIAG:-/data_persist/log/aaw-dongle-reset.log}

log() { echo "reset-aa-dongle: $*"; }

# --- 0. which interface? -------------------------------------------------
# argv[1], else aa_iface from libpatch.conf, else wlan1.
IFACE=$1
if [ -z "$IFACE" ] && [ -r "$LIBPATCH_CONF" ]; then
    IFACE=$(sed -n 's/^[[:space:]]*aa_iface[[:space:]]*=[[:space:]]*\([^[:space:]#]*\).*/\1/p' \
            "$LIBPATCH_CONF" | tail -1)
    [ -z "$IFACE" ] && echo "reset-aa-dongle: WARNING: could not read aa_iface from" \
                            "$LIBPATCH_CONF — falling back to wlan1. If aa_iface is NOT wlan1," \
                            "pass it as argv[1]."
fi
[ -z "$IFACE" ] && IFACE=wlan1
AP_CONF=aa-hostapd-$IFACE.conf
DHCP_CONF=aa-udhcpd.conf

log "start (iface=$IFACE)"

if [ ! -e "/sys/class/net/$IFACE" ]; then
    log "ERROR: /sys/class/net/$IFACE does not exist — the interface is already gone."
    log "       ABORTING: that needs the driver loader (load-wlan1.sh) or a replug, not a rebind."
    exit 1
fi

# --- 1. USB interface and bound driver, from sysfs -------------------------
# /sys/class/net/<iface>/device is the USB interface (e.g. "2-1.3:1.0"), which
# is what the driver's bind/unbind files take.
DEVLINK=$(readlink "/sys/class/net/$IFACE/device")
INTF=${DEVLINK##*/}
DRVLINK=$(readlink "/sys/class/net/$IFACE/device/driver")
DRV=${DRVLINK##*/}
if [ -z "$INTF" ] || [ -z "$DRV" ]; then
    log "ERROR: cannot resolve $IFACE's USB interface / driver from sysfs"
    log "       (device=\"$DEVLINK\" driver=\"$DRVLINK\"). ABORTING."
    exit 1
fi
if [ ! -e "/sys/bus/usb/drivers/$DRV/unbind" ]; then
    log "ERROR: /sys/bus/usb/drivers/$DRV/unbind absent — $IFACE is bound to \"$DRV\","
    log "       which is not a USB driver we can rebind. ABORTING."
    exit 1
fi
log "dongle is $INTF on driver $DRV (MAC $(cat "/sys/class/net/$IFACE/address" 2>/dev/null))"

# --- 2. record the state before the reset --------------------------------
# Generic tools first, then the driver's /proc tree if it has one. Status files
# only: the register dumps issue hundreds of USB reads and can hang on a stuck
# chip.
PROC=/proc/net/$DRV/$IFACE
{
    # Same boot key as start-aa-ap.sh (the clock restarts at 1970 every boot).
    echo "=========== $(date) : AAW dongle reset ($IFACE / $DRV / $INTF) boot_id=$(cut -c1-8 /proc/sys/kernel/random/boot_id 2>/dev/null) ==========="
    echo "--- ifconfig $IFACE";  ifconfig "$IFACE" 2>&1
    echo "--- /proc/net/dev";    grep "$IFACE" /proc/net/dev 2>&1
    echo "--- iw dev $IFACE info";         iw dev "$IFACE" info 2>&1
    echo "--- iw dev $IFACE station dump"; iw dev "$IFACE" station dump 2>&1
    echo "--- hostapd pids: $(pidof hostapd 2>/dev/null)"
    echo "--- udhcpd  pids: $(pidof udhcpd 2>/dev/null)"
    echo "--- leases";           cat /tmp/aa-udhcpd.leases 2>&1
    if [ -d "$PROC" ]; then
        for f in adapter_state fwstate mlmext_state rf_info ht_option sec_info trx_info all_sta_info; do
            [ -r "$PROC/$f" ] && { echo "--- $PROC/$f"; cat "$PROC/$f" 2>&1; }
        done
    else
        echo "--- $PROC absent (driver has no proc-debug tree) — iw output above is the record"
    fi
    echo "--- dmesg tail";       dmesg 2>/dev/null | tail -60
} >> "$DIAG" 2>&1
log "pre-reset diagnostics appended to $DIAG"


# --- 3. stop our udhcpd and hostapd -------------------------------------------
# Matched by config path in /proc/<pid>/cmdline; the unit's own hostapd (OEM
# Wi-Fi) is left alone.
for name in udhcpd hostapd; do
    case "$name" in
        udhcpd)  want=$DHCP_CONF ;;
        hostapd) want=$AP_CONF ;;
    esac
    found=0
    for pid in $(pidof "$name" 2>/dev/null); do
        cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
        case "$cmd" in
            *"$want"*) log "stopping our $name (pid $pid)"; kill "$pid" 2>/dev/null; found=1 ;;
            *)         log "leaving $name pid $pid alone — not ours ($cmd)" ;;
        esac
    done
    [ "$found" = 0 ] && log "our $name ($want) was not running"
done

# --- 4. re-probe the chip -----------------------------------------------------
log "unbind $INTF from $DRV"
if ! echo "$INTF" > "/sys/bus/usb/drivers/$DRV/unbind" 2>/dev/null; then
    log "ERROR: unbind write failed. ABORTING."
    exit 1
fi
# The interface disappearing means the driver has released the chip.
i=0
while [ -e "/sys/class/net/$IFACE" ] && [ "$i" -lt 10 ]; do sleep 1; i=$((i + 1)); done
if [ -e "/sys/class/net/$IFACE" ]; then
    log "ERROR: $IFACE still present ${i}s after unbind — the driver did not release the"
    log "       chip (something still holds it). ABORTING without a bind."
    exit 1
fi
log "$IFACE gone after ${i}s — chip released"

log "bind $INTF back to $DRV (re-runs the full probe: FW download, efuse, PHY/RF config)"
if ! echo "$INTF" > "/sys/bus/usb/drivers/$DRV/bind" 2>/dev/null; then
    log "ERROR: bind write failed — $IFACE will NOT come back without a replug. ABORTING."
    exit 1
fi

i=0
while [ ! -e "/sys/class/net/$IFACE" ] && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
if [ ! -e "/sys/class/net/$IFACE" ]; then
    log "ERROR: $IFACE did not reappear ${i}s after bind."
    log "       wlanN now present: $(ls -d /sys/class/net/wlan* 2>/dev/null)"
    log "       ABORTING — the AP is DOWN; this needs a physical replug or a reboot."
    exit 1
fi
log "$IFACE back after ${i}s (MAC $(cat "/sys/class/net/$IFACE/address" 2>/dev/null))"

# --- 5. restart the AP ------------------------------------------------------
if [ ! -x "$AP_START" ]; then
    log "ERROR: $AP_START missing or not executable — cannot restart the AP. ABORTING."
    exit 1
fi
log "re-running $AP_START"
"$AP_START"

# Check that our hostapd is running.
ok=0
for pid in $(pidof hostapd 2>/dev/null); do
    if tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q "$AP_CONF"; then ok=1; fi
done

# Record the state after the reset in the background. aaw_listend holds back
# handshakes while this script runs, so any delay here costs the phone a retry.
# The record waits until hostapd's start-up channel scan is over
# (WIFI_SITE_MONITOR, 0x800, clear in fwstate); a record taken during the scan
# looks like a broken radio.
(
    if [ -r "$PROC/fwstate" ]; then
        i=0
        while [ "$i" -lt 10 ]; do
            # Test the bit arithmetically; $(( )) accepts the 0x prefix.
            fw=$(sed -n 's/^fwstate=//p' "$PROC/fwstate" 2>/dev/null)
            case "$fw" in
                0x*) [ $(( fw & 0x800 )) -eq 0 ] && break ;;
                *)   break ;;
            esac
            sleep 1; i=$((i + 1))
        done
    fi
    echo "--- POST-RESET $(date) (ours running: $ok, settled after ${i:-0}s, fwstate=${fw:-n/a})"
    echo "--- ifconfig $IFACE";      ifconfig "$IFACE" 2>&1
    echo "--- iw dev $IFACE info";   iw dev "$IFACE" info 2>&1
    echo "--- iw dev $IFACE station dump"; iw dev "$IFACE" station dump 2>&1
    echo "--- hostapd pids: $(pidof hostapd 2>/dev/null)"
    if [ -d "$PROC" ]; then
        for f in adapter_state fwstate mlmext_state rf_info trx_info all_sta_info; do
            [ -r "$PROC/$f" ] && { echo "--- $PROC/$f"; cat "$PROC/$f" 2>&1; }
        done
    fi
) >> "$DIAG" 2>&1 &

if [ "$ok" = 1 ]; then
    log "done, AP is back up on $IFACE"
    exit 0
fi
log "ERROR: $AP_START ran but no hostapd is serving /tmp/$AP_CONF."
log "       hostapd pids: $(pidof hostapd 2>/dev/null) — the AP is DOWN."
exit 1
