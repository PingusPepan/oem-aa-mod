// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Android Auto Wireless (AAW) Bluetooth bootstrap protocol: message IDs,
// protobuf encoders and RFCOMM framing, following the aasdk AAW protobuf
// definitions. The head unit speaks it over the AAW RFCOMM channel to hand the
// phone the Wi-Fi credentials and the AA TCP endpoint.

#ifndef LIBPATCH_TOOLS_AAW_H
#define LIBPATCH_TOOLS_AAW_H

#include <stdint.h>
#include <string.h>

namespace aaw {

// Message IDs (aasdk aap_protobuf/aaw/MessageId.proto). The phone opens the
// RFCOMM channel and waits; the head unit speaks first:
//   HU->VERSION_REQUEST  phone->VERSION_RESPONSE
//   HU->START_REQUEST    phone->INFO_REQUEST  HU->INFO_RESPONSE
//   phone->START_RESPONSE, then CONNECTION_STATUS
enum MessageId {
    WIFI_START_REQUEST     = 1,   // HU -> phone : {ip_address, port}
    WIFI_INFO_REQUEST      = 2,   // phone -> HU : {}
    WIFI_INFO_RESPONSE     = 3,   // HU -> phone : {ssid, password, bssid, security}
    WIFI_VERSION_REQUEST   = 4,   // HU -> phone : {}
    WIFI_VERSION_RESPONSE  = 5,   // phone -> HU : {a,b,[c],d}
    WIFI_CONNECTION_STATUS = 6,   // phone -> HU : {status}
    WIFI_START_RESPONSE    = 7,   // phone -> HU : {status}
};

// WifiSecurityMode; the same values cc_wifi_init takes.
enum SecurityMode { OPEN = 1, WPA2_PERSONAL = 5 };

// Status (aasdk aap_protobuf/aaw/Status.proto), the `status` field of
// WifiConnectionStatus and WifiStartResponse. It is a proto2 int32, so failures
// are negative and arrive as 10-byte varints; decode with pb_varint_s().
//
// STATUS_WIFI_NETWORK_UNAVAILABLE means the phone scanned and did not find the
// AP. It is the only reliable sign that the dongle is not transmitting: the
// head unit has no second 5 GHz radio to check with, and the driver reports a
// healthy state even when the radio is silent.
enum Status {
    STATUS_UNSOLICITED_MESSAGE              =  1,
    STATUS_SUCCESS                          =  0,
    STATUS_NO_COMPATIBLE_VERSION            = -1,
    STATUS_WIFI_INACCESSIBLE_CHANNEL        = -2,
    STATUS_WIFI_INCORRECT_CREDENTIALS       = -3,
    STATUS_PROJECTION_ALREADY_STARTED       = -4,
    STATUS_WIFI_DISABLED                    = -5,
    STATUS_WIFI_NOT_YET_STARTED             = -6,
    STATUS_INVALID_HOST                     = -7,
    STATUS_NO_SUPPORTED_WIFI_CHANNELS       = -8,
    STATUS_INSTRUCT_USER_TO_CHECK_THE_PHONE = -9,
    STATUS_PHONE_WIFI_DISABLED              = -10,
    STATUS_WIFI_NETWORK_UNAVAILABLE         = -11,
};

inline const char *status_name(int s)
{
    switch (s) {
    case STATUS_UNSOLICITED_MESSAGE:              return "UNSOLICITED_MESSAGE";
    case STATUS_SUCCESS:                          return "SUCCESS";
    case STATUS_NO_COMPATIBLE_VERSION:            return "NO_COMPATIBLE_VERSION";
    case STATUS_WIFI_INACCESSIBLE_CHANNEL:        return "WIFI_INACCESSIBLE_CHANNEL";
    case STATUS_WIFI_INCORRECT_CREDENTIALS:       return "WIFI_INCORRECT_CREDENTIALS";
    case STATUS_PROJECTION_ALREADY_STARTED:       return "PROJECTION_ALREADY_STARTED";
    case STATUS_WIFI_DISABLED:                    return "WIFI_DISABLED";
    case STATUS_WIFI_NOT_YET_STARTED:             return "WIFI_NOT_YET_STARTED";
    case STATUS_INVALID_HOST:                     return "INVALID_HOST";
    case STATUS_NO_SUPPORTED_WIFI_CHANNELS:       return "NO_SUPPORTED_WIFI_CHANNELS";
    case STATUS_INSTRUCT_USER_TO_CHECK_THE_PHONE: return "INSTRUCT_USER_TO_CHECK_THE_PHONE";
    case STATUS_PHONE_WIFI_DISABLED:              return "PHONE_WIFI_DISABLED";
    case STATUS_WIFI_NETWORK_UNAVAILABLE:         return "WIFI_NETWORK_UNAVAILABLE";
    default:                                      return "?";
    }
}

// aaw_handler's exit status, read by aaw_listend. The handler usually exits on
// its own right after the phone reports its status and drops the channel, so
// the exit code is how the verdict reaches the supervisor.
enum HandlerExit {
    HANDLER_OK             = 0,
    HANDLER_AP_UNREACHABLE = 11,   // phone could not find the AP (STATUS_WIFI_NETWORK_UNAVAILABLE = -11)
    HANDLER_SESSION_OK     = 12,   // phone joined the AP and connected
};

// Minimal protobuf2 writer; the AAW messages are small and flat.
struct Writer {
    uint8_t  buf[512];
    unsigned len;
    Writer() : len(0) {}
    void put(uint8_t b)        { if (len < sizeof(buf)) buf[len++] = b; }
    void varint(uint64_t v)    { do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; put(b); } while (v); }
    void tag(unsigned f, unsigned w) { varint((uint64_t)(f << 3) | w); }
    void u(unsigned f, uint64_t v)   { tag(f, 0); varint(v); }
    void s(unsigned f, const char *str) {
        unsigned n = str ? (unsigned)strlen(str) : 0;
        tag(f, 2); varint(n);
        for (unsigned i = 0; i < n; ++i) put((uint8_t)str[i]);
    }
};

// ---- message builders (w must be empty) ----------------------------------
inline void build_wifi_start_request(Writer &w, const char *ip, uint32_t port) {
    w.s(1, ip);      // ip_address
    w.u(2, port);    // port
}
inline void build_wifi_info_response(Writer &w, const char *ssid, const char *pwd,
                                     const char *bssid, uint32_t security) {
    w.s(1, ssid);        // ssid
    w.s(2, pwd);         // password
    w.s(3, bssid);       // bssid
    w.u(4, security);    // security_mode (WifiSecurityMode)
}
inline void build_wifi_version_response(Writer &w) {
    // aasdk names these fields "unknown"; these are the values phones accept.
    w.u(1, 1);   // unknown_value_a
    w.u(2, 1);   // unknown_value_b
    w.u(4, 2);   // unknown_value_d
}

// ---- RFCOMM framing ------------------------------------------------------
// Frame: [u16 BE length][u16 BE message_id][protobuf payload]. The length
// counts the payload only, not the message id; counting the id as well leaves
// the phone waiting for two more bytes, and it never replies. Returns the
// framed length, or 0 if it does not fit.
inline unsigned frame(uint8_t *out, unsigned out_sz, uint16_t msg_id,
                      const uint8_t *payload, unsigned payload_len) {
    unsigned total = 4 + payload_len;
    if (total > out_sz) return 0;
    out[0] = (uint8_t)(payload_len >> 8);  out[1] = (uint8_t)(payload_len & 0xff);
    out[2] = (uint8_t)(msg_id >> 8); out[3] = (uint8_t)(msg_id & 0xff);
    if (payload_len) memcpy(out + 4, payload, payload_len);
    return total;
}

// Parse a frame header. Returns the payload and sets msg_id/payload_len, or
// returns nullptr if the frame is not complete yet.
inline const uint8_t *parse(const uint8_t *in, unsigned in_len,
                            uint16_t *msg_id, unsigned *payload_len) {
    if (in_len < 4) return nullptr;
    unsigned plen = ((unsigned)in[0] << 8) | in[1];
    if ((unsigned)(4 + plen) > in_len) return nullptr;
    *msg_id      = (uint16_t)(((unsigned)in[2] << 8) | in[3]);
    *payload_len = plen;
    return in + 4;
}

// Read varint field `field` as a signed 64-bit value; false if absent or
// truncated. Negative int32 values arrive as 10-byte two's-complement varints
// (STATUS_WIFI_NETWORK_UNAVAILABLE = 08 f5 ff ff ff ff ff ff ff ff 01).
// Length-delimited fields are skipped assuming a one-byte length.
inline bool pb_varint_s(const uint8_t *buf, unsigned len, unsigned field, int64_t *out)
{
    const uint8_t want = (uint8_t)((field << 3) | 0);   // wire type 0 = varint
    unsigned i = 0;
    while (i < len) {
        uint8_t t  = buf[i++];
        unsigned wt = t & 7;
        if (t == want) {
            uint64_t v = 0; unsigned sh = 0;
            while (i < len && sh < 64) {
                uint8_t b = buf[i++];
                v |= (uint64_t)(b & 0x7f) << sh;
                if (!(b & 0x80)) { *out = (int64_t)v; return true; }
                sh += 7;
            }
            return false;
        }
        if (wt == 0)      { while (i < len && (buf[i++] & 0x80)) {} }
        else if (wt == 2) { if (i >= len) break; i += (unsigned)buf[i] + 1; }
        else if (wt == 5) i += 4;
        else if (wt == 1) i += 8;
        else break;
    }
    return false;
}

} // namespace aaw

#endif // LIBPATCH_TOOLS_AAW_H
