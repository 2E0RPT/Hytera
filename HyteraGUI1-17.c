#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define REPEATER_SNMP_PORT 161
#define WM_TELEMETRY_READY (WM_USER + 101)

// Dynamic global timing interval modified by the UI window menu bar subsystem
volatile LONG g_PollIntervalMs = 5000;

// Repeater IP, set once in WinMain from argv[1] -- needed both by the
// telemetry thread and by WM_COMMAND (Admin > RESET > ... > REBOOT NOW).
static char g_repeaterIp[64];

typedef enum { TELEM_FLOAT, TELEM_INT, TELEM_ALARM_TRISTATE, TELEM_ALARM_BISTATE, TELEM_STRING } telem_type_t;

typedef struct {
    const char* label;
    uint8_t branch;
    uint8_t item;
    telem_type_t type;
    const char* unit;
} telem_oid_t;

static const telem_oid_t TELEM_OIDS[] = {
    { "Voltage",       2, 1,  TELEM_FLOAT,          "V"  }, // 0
    { "Voltage Alarm", 1, 1,  TELEM_ALARM_TRISTATE,  ""  }, // 1
    { "PA Temp",       2, 2,  TELEM_FLOAT,          "C"  }, // 2
    { "Temp Alarm",    1, 2,  TELEM_ALARM_TRISTATE,  ""  }, // 3
    { "VSWR",          2, 4,  TELEM_FLOAT,           ""  }, // 4
    { "VSWR Alarm",    1, 6,  TELEM_ALARM_BISTATE,   ""  }, // 5
    { "Battery Alarm", 1, 9,  TELEM_ALARM_BISTATE,   ""  }, // 6
    { "Slot1 RSSI",    2, 9,  TELEM_INT,             "dB" }, // 7
    { "Slot2 RSSI",    2, 10, TELEM_INT,             "dB" }, // 8
};
#define TELEM_COUNT (sizeof(TELEM_OIDS) / sizeof(TELEM_OIDS[0]))

// rptChannelName lives under rptSystemInfo (.1.2.4.9.0), a different branch
// to everything in TELEM_OIDS above (which are all rptRealTimeInfo, .1.2.1.x).
// build_telem_oid_content() below hardcodes the rptRealTimeInfo shallow
// prefix, so it can't express this OID's path -- it gets its own fixed byte
// array instead, appended to the same combined GetRequest as one extra
// varbind. Bytes confirmed against a real Wireshark capture (see Diag7.c,
// github.com/2E0RPT/Hytera) and cross-checked by hand-deriving the same
// bytes independently from the MIB -- both agree byte-for-byte.
static const uint8_t CHANNEL_NAME_OID_CONTENT[] = {
    0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02, 0x04, 0x09, 0x00
};

typedef struct {
    float floats[TELEM_COUNT];
    long ints[TELEM_COUNT];
    int is_float[TELEM_COUNT];
    char channel_name[64];
    long channel_number;   // rptChannelNumber, .1.2.2.2.0, INTEGER(0..15)
    long tx_power_level;   // rptTxPowerLevel, .1.2.2.5.0, INTEGER: high=0, low=2
    BOOL is_online;
} TelemetryData;

typedef struct {
    char target_ip[64];
    HWND hMainWnd;
} ThreadParams;

// --- BER Format Parsing Core Helper Methods ---

static int ber_write_length(uint8_t* buf, int offset, int len) {
    if (len < 128) { buf[offset] = (uint8_t)len; return 1; }
    else if (len < 256) { buf[offset] = 0x81; buf[offset + 1] = (uint8_t)len; return 2; }
    else { buf[offset] = 0x82; buf[offset + 1] = (uint8_t)(len >> 8); buf[offset + 2] = (uint8_t)(len & 0xFF); return 3; }
}

static int ber_append_tlv(uint8_t* dest, int dest_offset, uint8_t tag, const uint8_t* content, int content_len) {
    dest[dest_offset] = tag;
    int len_bytes = ber_write_length(dest, dest_offset + 1, content_len);
    int header_len = 1 + len_bytes;
    if (content_len > 0) memcpy(dest + dest_offset + header_len, content, content_len);
    return dest_offset + header_len + content_len;
}

static int ber_read_tlv(const uint8_t* buf, int buf_len, int* offset, uint8_t* out_tag, const uint8_t** out_content, int* out_content_len) {
    if (*offset >= buf_len) return 0;
    uint8_t tag = buf[*offset]; (*offset)++;
    if (*offset >= buf_len) return 0;
    uint8_t len_byte = buf[*offset]; (*offset)++;
    int content_len;
    if (len_byte & 0x80) {
        int num_len_bytes = len_byte & 0x7F;
        if (num_len_bytes == 0 || num_len_bytes > 4 || *offset + num_len_bytes > buf_len) return 0;
        content_len = 0;
        for (int i = 0; i < num_len_bytes; i++) { content_len = (content_len << 8) | buf[*offset]; (*offset)++; }
    } else { content_len = len_byte; }
    if (content_len < 0 || *offset + content_len > buf_len) return 0;
    *out_tag = tag; *out_content = buf + *offset; *out_content_len = content_len; *offset += content_len;
    return 1;
}

static long ber_decode_integer(const uint8_t* content, int len) {
    long value = (len > 0 && (content[0] & 0x80)) ? -1 : 0;
    for (int i = 0; i < len; i++) value = (value << 8) | content[i];
    return value;
}

static float decode_hytera_float(const uint8_t* content, int len) {
    float f = 0.0f; if (len == 4) memcpy(&f, content, 4); return f;
}

// Sends a hand-built SNMPv1 SetRequest to Hytera's private rptRestart OID.
// Ported from the confirmed-working HyteraTransceiver1-9 version -- same
// fixed 46-byte BER/ASN.1 packet, confirmed directly against
// HYTERA-REPEATER-MIB (OID .1.3.6.1.4.1.40297.1.2.2.1.0, community
// "public", value 1 = reset) and verified working via iReasoning MIB
// Browser and a real repeater reboot. Only change from that version:
// printf diagnostics replaced with MessageBoxA, since HyteraGUI is a
// windowed app with no console attached to print to.
void send_snmp_repeater_restart(const char* target_ip) {
    static const uint8_t SNMP_RESTART_PACKET[] = {
        0x30, 0x2C,                                   // SEQUENCE, message, len 44
          0x02, 0x01, 0x00,                             // INTEGER version = 0 (SNMPv1)
          0x04, 0x06, 'p','u','b','l','i','c',          // OCTET STRING community = "public"
          0xA3, 0x1F,                                   // SetRequest-PDU, len 31
            0x02, 0x01, 0x01,                            // INTEGER request-id = 1
            0x02, 0x01, 0x00,                            // INTEGER error-status = 0
            0x02, 0x01, 0x00,                            // INTEGER error-index = 0
            0x30, 0x14,                                  // SEQUENCE variable-bindings, len 20
              0x30, 0x12,                                 // SEQUENCE VarBind, len 18
                0x06, 0x0D, 0x2B, 0x06, 0x01, 0x04, 0x01, // OID .1.3.6.1.4.1.
                      0x82, 0xBA, 0x69,                   // .40297 (base-128)
                      0x01, 0x02, 0x02, 0x01, 0x00,       // .1.2.2.1.0
                0x02, 0x01, 0x01                          // INTEGER value = 1 (reset)
    };

    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) {
        MessageBoxA(NULL, "Could not create SNMP socket.", "Reboot Failed", MB_OK | MB_ICONERROR);
        return;
    }

    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(161); // Standard SNMP agent port
    inet_pton(AF_INET, target_ip, &snmp_addr.sin_addr);

    int sent = sendto(snmp_sock, (const char*)SNMP_RESTART_PACKET, sizeof(SNMP_RESTART_PACKET), 0,
                       (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));
    if (sent == SOCKET_ERROR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to send SNMP restart command (WSAGetLastError=%d).", WSAGetLastError());
        MessageBoxA(NULL, msg, "Reboot Failed", MB_OK | MB_ICONERROR);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "rptRestart SetRequest sent to %s:161.", target_ip);
        MessageBoxA(NULL, msg, "Reboot Command Sent", MB_OK | MB_ICONINFORMATION);
    }

    closesocket(snmp_sock);
}

// Decodes a Hytera "unicode string" (UTF-16LE) OCTET STRING into a plain
// char buffer by keeping only the low byte of each 16-bit code unit and
// discarding the high byte. Confirmed correct against a real repeater
// response in Diag7.c. This only handles characters in the ASCII/Latin-1
// range correctly (true multi-byte Unicode, e.g. CJK, would come out
// wrong) -- fine for channel names, which are effectively always plain
// ASCII in practice.
static void decode_hytera_unicode_string(const uint8_t* content, int content_len, char* out, int out_size) {
    int write_index = 0;
    for (int j = 0; j < content_len; j += 2) {
        if (write_index < out_size - 1) {
            out[write_index] = (char)content[j];
            write_index++;
        }
    }
    out[write_index] = '\0';
}

static void build_telem_oid_content(uint8_t* out, uint8_t branch, uint8_t item) {
    static const uint8_t PREFIX[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02, 0x01 };
    memcpy(out, PREFIX, sizeof(PREFIX)); out[sizeof(PREFIX) + 0] = branch; out[sizeof(PREFIX) + 1] = item; out[sizeof(PREFIX) + 2] = 0x00;
}

// rptControl-branch OID content (.1.3.6.1.4.1.40297.1.2.2.<item>.0) -- same
// shallow prefix confirmed working for rptRestart, no alarm/data split like
// rptRealTimeInfo above. Used for rptChannelNumber (item=2) and
// rptTxPowerLevel (item=5).
static void build_control_oid_content(uint8_t* out, uint8_t item) {
    static const uint8_t PREFIX[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02 };
    memcpy(out, PREFIX, sizeof(PREFIX)); out[sizeof(PREFIX) + 0] = 0x02; out[sizeof(PREFIX) + 1] = item; out[sizeof(PREFIX) + 2] = 0x00;
}

// Sends a SNMPv1 SetRequest for a single rptControl-branch INTEGER OID
// (.1.3.6.1.4.1.40297.1.2.2.<item>.0), value 0-15 (fits in one byte for
// every OID this is used with -- rptChannelNumber is 0..15, rptTxPowerLevel
// is 0 or 2). Built dynamically with the same BER helpers used for the
// GetRequest above, rather than a fixed byte array like the reboot
// command, since the value here varies per call.
void send_snmp_set_control_integer(const char* target_ip, uint8_t item, int value) {
    uint8_t oid_content[13];
    build_control_oid_content(oid_content, item);
    uint8_t oid_tlv[16];
    int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, oid_content, sizeof(oid_content));

    uint8_t value_byte = (uint8_t)(value & 0xFF);
    uint8_t value_tlv[3];
    int value_len = ber_append_tlv(value_tlv, 0, 0x02, &value_byte, 1); // INTEGER

    uint8_t varbind_content[24]; int vc_len = 0;
    memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
    memcpy(varbind_content + vc_len, value_tlv, value_len); vc_len += value_len;
    uint8_t varbind_tlv[32];
    int vb_len = ber_append_tlv(varbind_tlv, 0, 0x30, varbind_content, vc_len);

    uint8_t varbind_list_tlv[40];
    int vbl_len = ber_append_tlv(varbind_list_tlv, 0, 0x30, varbind_tlv, vb_len);

    uint8_t reqid[3] = { 0x02, 0x01, 0x01 };
    uint8_t errstat[3] = { 0x02, 0x01, 0x00 };
    uint8_t errindex[3] = { 0x02, 0x01, 0x00 };
    uint8_t pdu_content[56]; int pc_len = 0;
    memcpy(pdu_content + pc_len, reqid, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errstat, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errindex, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, varbind_list_tlv, vbl_len); pc_len += vbl_len;
    uint8_t pdu_tlv[64];
    int pdu_len = ber_append_tlv(pdu_tlv, 0, 0xA3, pdu_content, pc_len); // SetRequest-PDU

    uint8_t version[3] = { 0x02, 0x01, 0x00 };
    uint8_t community[8] = { 0x04, 0x06, 'p','u','b','l','i','c' };
    uint8_t msg_content[80]; int mc_len = 0;
    memcpy(msg_content + mc_len, version, 3); mc_len += 3;
    memcpy(msg_content + mc_len, community, 8); mc_len += 8;
    memcpy(msg_content + mc_len, pdu_tlv, pdu_len); mc_len += pdu_len;

    uint8_t packet[100];
    int packet_len = ber_append_tlv(packet, 0, 0x30, msg_content, mc_len);

    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) return;
    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(161);
    inet_pton(AF_INET, target_ip, &snmp_addr.sin_addr);
    sendto(snmp_sock, (const char*)packet, packet_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));
    closesocket(snmp_sock);
}

static int build_telem_getrequest(uint8_t* out_buf, int request_id) {
    uint8_t varbind_list[1200]; int vb_offset = 0;

    for (size_t i = 0; i < TELEM_COUNT; i++) {
        uint8_t oid_content[14]; build_telem_oid_content(oid_content, TELEM_OIDS[i].branch, TELEM_OIDS[i].item);
        uint8_t oid_tlv[16]; int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, oid_content, sizeof(oid_content));
        uint8_t null_tlv[2] = { 0x05, 0x00 };
        uint8_t varbind_content[32]; int vc_len = 0;
        memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
        memcpy(varbind_content + vc_len, null_tlv, sizeof(null_tlv)); vc_len += sizeof(null_tlv);
        vb_offset = ber_append_tlv(varbind_list, vb_offset, 0x30, varbind_content, vc_len);
    }

    // Channel name varbind -- appended after the main telemetry loop so it
    // rides along in the same single GetRequest/GetResponse round-trip.
    {
        uint8_t oid_tlv[16];
        int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, CHANNEL_NAME_OID_CONTENT, sizeof(CHANNEL_NAME_OID_CONTENT));
        uint8_t null_tlv[2] = { 0x05, 0x00 };
        uint8_t varbind_content[32]; int vc_len = 0;
        memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
        memcpy(varbind_content + vc_len, null_tlv, sizeof(null_tlv)); vc_len += sizeof(null_tlv);
        vb_offset = ber_append_tlv(varbind_list, vb_offset, 0x30, varbind_content, vc_len);
    }

    // rptChannelNumber (item 2) and rptTxPowerLevel (item 5) -- needed so
    // CH+/CH-/PWR H-L know the current value before computing the new one.
    {
        uint8_t items[2] = { 2, 5 };
        for (int k = 0; k < 2; k++) {
            uint8_t oid_content[13];
            build_control_oid_content(oid_content, items[k]);
            uint8_t oid_tlv[16];
            int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, oid_content, sizeof(oid_content));
            uint8_t null_tlv[2] = { 0x05, 0x00 };
            uint8_t varbind_content[32]; int vc_len = 0;
            memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
            memcpy(varbind_content + vc_len, null_tlv, sizeof(null_tlv)); vc_len += sizeof(null_tlv);
            vb_offset = ber_append_tlv(varbind_list, vb_offset, 0x30, varbind_content, vc_len);
        }
    }

    uint8_t varbind_list_tlv[1220]; int vbl_len = ber_append_tlv(varbind_list_tlv, 0, 0x30, varbind_list, vb_offset);
    uint8_t reqid[3] = { 0x02, 0x01, (uint8_t)(request_id & 0xFF) };
    uint8_t errstat[3] = { 0x02, 0x01, 0x00 }; uint8_t errindex[3] = { 0x02, 0x01, 0x00 };
    uint8_t pdu_content[1240]; int pc_len = 0;
    memcpy(pdu_content + pc_len, reqid, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errstat, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errindex, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, varbind_list_tlv, vbl_len); pc_len += vbl_len;
    uint8_t pdu_tlv[1250]; int pdu_len = ber_append_tlv(pdu_tlv, 0, 0xA0, pdu_content, pc_len);
    uint8_t version[3] = { 0x02, 0x01, 0x00 }; uint8_t community[8] = { 0x04, 0x06, 'p','u','b','l','i','c' };
    uint8_t msg_content[1270]; int mc_len = 0;
    memcpy(msg_content + mc_len, version, 3); mc_len += 3;
    memcpy(msg_content + mc_len, community, 8); mc_len += 8;
    memcpy(msg_content + mc_len, pdu_tlv, pdu_len); mc_len += pdu_len;
    return ber_append_tlv(out_buf, 0, 0x30, msg_content, mc_len);
}

static int parse_telem_getresponse(const uint8_t* buf, int buf_len, TelemetryData* outData) {
    int off = 0; uint8_t tag; const uint8_t* content; int clen;
    if (!ber_read_tlv(buf, buf_len, &off, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* msg = content; int msg_len = clen; int mo = 0;
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // version
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // community
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // PDU
    if (tag != 0xA2) return 0;
    const uint8_t* pdu = content; int pdu_len = clen; int po = 0;
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0; // request-id
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0; // error-status
    long error_status = ber_decode_integer(content, clen);
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0; // error-index
    if (error_status != 0) return 0;
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* vbl = content; int vbl_len = clen; int vo = 0;

    for (size_t i = 0; i < TELEM_COUNT; i++) {
        if (!ber_read_tlv(vbl, vbl_len, &vo, &tag, &content, &clen) || tag != 0x30) return 0;
        const uint8_t* vb = content; int vb_len = clen; int vbo = 0;
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // OID
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // value
        if (tag == 0x04) { outData->floats[i] = decode_hytera_float(content, clen); outData->is_float[i] = 1; }
        else if (tag == 0x02) { outData->ints[i] = ber_decode_integer(content, clen); outData->is_float[i] = 0; }
        else { return 0; }
    }

    // 10th varbind: channel name (UTF-16LE OCTET STRING)
    if (!ber_read_tlv(vbl, vbl_len, &vo, &tag, &content, &clen) || tag != 0x30) return 0;
    {
        const uint8_t* vb = content; int vb_len = clen; int vbo = 0;
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // OID
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // value
        if (tag == 0x04) {
            decode_hytera_unicode_string(content, clen, outData->channel_name, sizeof(outData->channel_name));
        } else {
            outData->channel_name[0] = '\0'; // Not a string back -- leave blank rather than garbage
        }
    }

    // 11th varbind: rptChannelNumber, 12th: rptTxPowerLevel (both INTEGER)
    {
        long* targets[2] = { &outData->channel_number, &outData->tx_power_level };
        for (int k = 0; k < 2; k++) {
            if (!ber_read_tlv(vbl, vbl_len, &vo, &tag, &content, &clen) || tag != 0x30) return 0;
            const uint8_t* vb = content; int vb_len = clen; int vbo = 0;
            if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // OID
            if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // value
            if (tag == 0x02) { *targets[k] = ber_decode_integer(content, clen); }
            else { return 0; }
        }
    }

    return 1;
}

// --- Asynchronous Network Communication Engine Thread Loop ---
unsigned __stdcall TelemetryThread(void* pArguments) {
    ThreadParams* params = (ThreadParams*)pArguments;
    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) return 0;
    DWORD snmp_timeout = 2000;
    setsockopt(snmp_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&snmp_timeout, sizeof(snmp_timeout));
    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(REPEATER_SNMP_PORT);
    inet_pton(AF_INET, params->target_ip, &snmp_addr.sin_addr);
    int request_id = 1;
    while (1) {
        uint8_t request[1300];
        int req_len = build_telem_getrequest(request, request_id++);
        sendto(snmp_sock, (const char*)request, req_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));
        uint8_t response[1500];
        struct sockaddr_in from_addr; int from_len = sizeof(from_addr);
        int n = recvfrom(snmp_sock, (char*)response, sizeof(response), 0, (struct sockaddr*)&from_addr, &from_len);
        TelemetryData* dataPayload = (TelemetryData*)malloc(sizeof(TelemetryData));
        if (dataPayload) {
            memset(dataPayload, 0, sizeof(TelemetryData));
            if (n > 0 && parse_telem_getresponse(response, n, dataPayload)) { dataPayload->is_online = TRUE; }
            else { dataPayload->is_online = FALSE; }
            PostMessage(params->hMainWnd, WM_TELEMETRY_READY, 0, (LPARAM)dataPayload);
        }
        Sleep(InterlockedExchangeAdd(&g_PollIntervalMs, 0));
    }
    closesocket(snmp_sock);
    free(params);
    return 0;
}

// --- Gauge Painting Helper System ---
void PaintAnalogGauge(HDC hdc, int x, int y, int w, int h, const char* name, float value, float minVal, float maxVal, BOOL isAlarm, const char* fmt) {
    HBRUSH bgBrush = CreateSolidBrush(isAlarm ? RGB(140, 20, 20) : RGB(228, 224, 210));
    RECT rc = { x, y, x + w, y + h };
    FillRect(hdc, &rc, bgBrush);
    DrawEdge(hdc, &rc, EDGE_SUNKEN, BF_RECT);
    DeleteObject(bgBrush);
    HPEN arcPen = CreatePen(PS_SOLID, 2, isAlarm ? RGB(230, 230, 230) : RGB(40, 40, 40));
    HGDIOBJ oldPen = SelectObject(hdc, arcPen);
    int cx = x + w / 2;
    int cy = y + h - 22;
    int radius = w / 2 - 18;
    Arc(hdc, cx - radius, cy - radius, cx + radius, cy + radius, cx + radius, cy, cx - radius, cy);
    float pct = (value - minVal) / (maxVal - minVal);
    if (pct < 0.0f) pct = 0.0f; if (pct > 1.0f) pct = 1.0f;
    float angle_rad = 3.14159265f * (1.0f - pct);
    int nx = cx + (int)(cosf(angle_rad) * (radius - 4));
    int ny = cy - (int)(sinf(angle_rad) * (radius - 4));
    HPEN needlePen = CreatePen(PS_SOLID, 2, isAlarm ? RGB(255, 255, 0) : RGB(220, 20, 20));
    SelectObject(hdc, needlePen);
    MoveToEx(hdc, cx, cy, NULL); LineTo(hdc, nx, ny);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, isAlarm ? RGB(255, 255, 255) : RGB(40, 40, 40));
    HFONT hMedFont = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    HGDIOBJ oldFont = SelectObject(hdc, hMedFont);
    UINT oldAlign = SetTextAlign(hdc, TA_CENTER);
    TextOutA(hdc, cx, y + h - 17, name, (int)strlen(name));
    char lblMin[16], lblMid[16], lblMax[16];
    float midVal = minVal + (maxVal - minVal) / 2.0f;
    if (minVal == (int)minVal && maxVal == (int)maxVal) {
        snprintf(lblMin, sizeof(lblMin), "%d", (int)minVal);
        snprintf(lblMid, sizeof(lblMid), "%d", (int)midVal);
        snprintf(lblMax, sizeof(lblMax), "%d", (int)maxVal);
    } else {
        snprintf(lblMin, sizeof(lblMin), "%.1f", minVal);
        snprintf(lblMid, sizeof(lblMid), "%.1f", midVal);
        snprintf(lblMax, sizeof(lblMax), "%.1f", maxVal);
    }
    TextOutA(hdc, cx - radius + 8, cy - 14, lblMin, (int)strlen(lblMin));
    TextOutA(hdc, cx, cy - radius - 12, lblMid, (int)strlen(lblMid));
    TextOutA(hdc, cx + radius - 8, cy - 14, lblMax, (int)strlen(lblMax));
    HFONT hValFont = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    SelectObject(hdc, hValFont);
    SetTextColor(hdc, isAlarm ? RGB(255, 255, 0) : RGB(0, 102, 204));
    char valStr[32];
    snprintf(valStr, sizeof(valStr), fmt, value);
    TextOutA(hdc, cx, cy - 16, valStr, (int)strlen(valStr));
    SetTextAlign(hdc, oldAlign);
    SelectObject(hdc, oldFont);
    DeleteObject(hMedFont);
    DeleteObject(hValFont);
    SelectObject(hdc, oldPen);
    DeleteObject(arcPen);
    DeleteObject(needlePen);
}

// --- Main Window Event Callback ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static TelemetryData currentTelem;
    static BOOL receivedFirstData = FALSE;
    switch (msg) {
        case WM_CREATE: {
            // Old PTT/TS1/TS2/DMR/FM/TEMP buttons removed -- channel name now
            // has its own persistent display (drawn in WM_PAINT) instead of
            // living behind the TEMP diagnostic button.
            // New control buttons, to the right of that channel display.
            // Not wired to any actual repeater command yet -- placeholders,
            // same as the old PTT/TS1/TS2/DMR/FM buttons were.
            // CH+/CH- narrowed (66px vs the original 90px) so the row's
            // total width lines up MSG TS 2's right edge with the Temp C
            // gauge's right edge (750+150=900). One gap (after CH-) is 9px
            // instead of 8 to absorb the odd pixel and hit that exactly.
            // RESET removed per request.
            CreateWindowA("BUTTON", "CH +",     WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 465, 210, 66, 45, hwnd, (HMENU)6001, NULL, NULL);
            CreateWindowA("BUTTON", "CH -",     WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 539, 210, 66, 45, hwnd, (HMENU)6002, NULL, NULL);
            CreateWindowA("BUTTON", "PWR H/L",  WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 614, 210, 90, 45, hwnd, (HMENU)6003, NULL, NULL);
            CreateWindowA("BUTTON", "MSG TS 1", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 712, 210, 90, 45, hwnd, (HMENU)6004, NULL, NULL);
            CreateWindowA("BUTTON", "MSG TS 2", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 810, 210, 90, 45, hwnd, (HMENU)6005, NULL, NULL);
            HMENU hMenuBar = CreateMenu(); HMENU hSubMenu = CreatePopupMenu();
            for (int i = 1; i <= 50; i++) {
                char menuItemText[32];
                snprintf(menuItemText, sizeof(menuItemText), "%.1f Seconds", i / 10.0f);
                AppendMenuA(hSubMenu, MF_STRING, 2000 + i, menuItemText);
            }
            AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hSubMenu, "Update");

            // Admin > RESET > ARE YOU SURE? > YES I AM SURE - REBOOT NOW
            HMENU hAreYouSureMenu = CreatePopupMenu();
            AppendMenuA(hAreYouSureMenu, MF_STRING, 7001, "YES I AM SURE - REBOOT NOW");
            HMENU hResetMenu = CreatePopupMenu();
            AppendMenuA(hResetMenu, MF_POPUP, (UINT_PTR)hAreYouSureMenu, "ARE YOU SURE?");
            HMENU hAdminMenu = CreatePopupMenu();
            AppendMenuA(hAdminMenu, MF_POPUP, (UINT_PTR)hResetMenu, "RESET");
            AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hAdminMenu, "Admin");

            SetMenu(hwnd, hMenuBar);
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId > 2000 && wmId <= 2050) {
                LONG targetMs = (wmId - 2000) * 100;
                InterlockedExchange(&g_PollIntervalMs, targetMs);
            }
            else if (wmId == 7001) {
                // ROBS PLACE MARKER FOR REPEATER RESET
                send_snmp_repeater_restart(g_repeaterIp);
            }
            else if (wmId == 6001 || wmId == 6002) { // CH + / CH -
                if (!receivedFirstData || !currentTelem.is_online) {
                    MessageBoxA(hwnd, "Waiting for telemetry before channel can be changed.", "Not Ready", MB_OK | MB_ICONWARNING);
                } else {
                    long newChannel = currentTelem.channel_number + (wmId == 6001 ? 1 : -1);
                    if (newChannel > 15) newChannel = 15; // rptChannelNumber is INTEGER(0..15)
                    if (newChannel < 0) newChannel = 0;
                    send_snmp_set_control_integer(g_repeaterIp, 2, (int)newChannel);
                }
            }
            else if (wmId == 6003) { // PWR H/L
                if (!receivedFirstData || !currentTelem.is_online) {
                    MessageBoxA(hwnd, "Waiting for telemetry before power level can be changed.", "Not Ready", MB_OK | MB_ICONWARNING);
                } else {
                    int newLevel = (currentTelem.tx_power_level == 0) ? 2 : 0; // high(0) <-> low(2)
                    send_snmp_set_control_integer(g_repeaterIp, 5, newLevel);
                    MessageBoxA(hwnd, (newLevel == 0) ? "TX Power High" : "TX Power Low", "TX Power Level Set", MB_OK | MB_ICONINFORMATION);
                }
            }
            // MSG TS1/MSG TS2 (IDs 6004-6005) are still placeholders --
            // no repeater command wired up yet.
            break;
        }
        case WM_TELEMETRY_READY: {
            TelemetryData* incoming = (TelemetryData*)lParam;
            if (incoming) {
                currentTelem = *incoming;
                receivedFirstData = TRUE;
                free(incoming);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            HBRUSH hChassisBrush = CreateSolidBrush(RGB(28, 28, 30));
            RECT clientRc; GetClientRect(hwnd, &clientRc); FillRect(hdc, &clientRc, hChassisBrush); DeleteObject(hChassisBrush);
            HBRUSH hLedBg = CreateSolidBrush(RGB(15, 15, 15));
            RECT rcLedL = { 30, 20, 450, 75 }; FillRect(hdc, &rcLedL, hLedBg); DrawEdge(hdc, &rcLedL, EDGE_SUNKEN, BF_RECT);
            RECT rcLedR = { 480, 20, 900, 75 }; FillRect(hdc, &rcLedR, hLedBg); DrawEdge(hdc, &rcLedR, EDGE_SUNKEN, BF_RECT);
            DeleteObject(hLedBg);
            SetBkMode(hdc, TRANSPARENT);
            HFONT hLedFont = CreateFontA(38, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Courier New");
            HGDIOBJ oldFont = SelectObject(hdc, hLedFont);
            if (receivedFirstData && currentTelem.is_online) {
                char strL[64];
                snprintf(strL, sizeof(strL), "S1:%3ld S2:%3ld", currentTelem.ints[7], currentTelem.ints[8]);
                SetTextColor(hdc, RGB(240, 60, 60));
                TextOutA(hdc, 50, 28, strL, (int)strlen(strL));
                SetTextColor(hdc, RGB(60, 240, 60));
                TextOutA(hdc, 500, 28, "SYSTEM ONLINE", 13);
            } else {
                SetTextColor(hdc, RGB(60, 20, 20));
                TextOutA(hdc, 50, 28, "S1:--- S2:---", 14);
                SetTextColor(hdc, RGB(240, 40, 40));
                TextOutA(hdc, 500, 28, "LINK OFFLINE", 12);
            }
            SelectObject(hdc, oldFont); DeleteObject(hLedFont);

            // Channel name display -- same box style/size as the top-right
            // LED readout (420 x 55), left edge aligned with the top-left
            // readout (x=30). Sits where the old button row used to be.
            HBRUSH hChanBg = CreateSolidBrush(RGB(15, 15, 15));
            RECT rcChan = { 30, 205, 450, 260 };
            FillRect(hdc, &rcChan, hChanBg);
            DrawEdge(hdc, &rcChan, EDGE_SUNKEN, BF_RECT);
            DeleteObject(hChanBg);

            HFONT hChanFont = CreateFontA(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Courier New");
            HGDIOBJ oldChanFont = SelectObject(hdc, hChanFont);
            if (receivedFirstData && currentTelem.is_online && currentTelem.channel_name[0] != '\0') {
                SetTextColor(hdc, RGB(60, 200, 240));
                TextOutA(hdc, 45, 218, currentTelem.channel_name, (int)strlen(currentTelem.channel_name));
            } else if (receivedFirstData && currentTelem.is_online) {
                SetTextColor(hdc, RGB(90, 90, 90));
                TextOutA(hdc, 45, 218, "(no channel name)", 18);
            } else {
                SetTextColor(hdc, RGB(60, 20, 20));
                TextOutA(hdc, 45, 218, "----------------", 16);
            }
            SelectObject(hdc, oldChanFont); DeleteObject(hChanFont);

            float s1_rssi = receivedFirstData ? (float)currentTelem.ints[7] : -120.0f;
            float s2_rssi = receivedFirstData ? (float)currentTelem.ints[8] : -120.0f;
            float vswr_val = receivedFirstData ? currentTelem.floats[4] : 1.0f;
            float volts_val = receivedFirstData ? currentTelem.floats[0] : 0.0f;
            float temp_val = receivedFirstData ? currentTelem.floats[2] : -20.0f;
            BOOL v_alarm = receivedFirstData ? (currentTelem.ints[1] != 0) : FALSE;
            BOOL t_alarm = receivedFirstData ? (currentTelem.ints[3] != 0) : FALSE;
            BOOL vswr_alarm = receivedFirstData ? (currentTelem.ints[5] != 0) : FALSE;
            PaintAnalogGauge(hdc, 30, 100, 150, 90, "TS-1 db", s1_rssi, -120.0f, -40.0f, FALSE, "%.0f");
            PaintAnalogGauge(hdc, 210, 100, 150, 90, "TS-2 db", s2_rssi, -120.0f, -40.0f, FALSE, "%.0f");
            PaintAnalogGauge(hdc, 390, 100, 150, 90, "VSWR", vswr_val, 1.0f, 5.0f, vswr_alarm, "%.2f");
            PaintAnalogGauge(hdc, 570, 100, 150, 90, "Volts", volts_val, 9.0f, 16.0f, v_alarm, "%.1fV");
            PaintAnalogGauge(hdc, 750, 100, 150, 90, "Temp C", temp_val, -20.0f, 120.0f, t_alarm, "%.1fC");
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_DESTROY: { PostQuitMessage(0); break; }
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (__argc < 2) {
        MessageBoxA(NULL, "Usage: hytera_gui.exe <repeater_ip>\nExample: hytera_gui.exe 192.168.1.167", "Missing Target Host Parameter", MB_OK | MB_ICONEXCLAMATION);
        return -1;
    }
    WSADATA wsaData; if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return -1;
    WNDCLASSEXA wc = {0}; wc.cbSize = sizeof(WNDCLASSEXA); wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.lpszClassName = "HyteraHardwarePanelClass"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassExA(&wc)) return -1;

    // Right margin should match the left margin (30px, same as every
    // gauge/box's left edge). Temp C gauge's right edge is at 750+150=900,
    // so the CLIENT area needs to be exactly 930px wide. AdjustWindowRectEx
    // converts that desired client size into the correct outer window size
    // for these exact style flags (title bar, borders, and -- since a menu
    // gets attached via SetMenu in WM_CREATE -- the menu bar too, via the
    // bMenu parameter) rather than guessing a fixed pixel number, which
    // would only be approximately right and would drift on a different
    // Windows theme/DPI.
    const DWORD winStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = { 0, 0, 930, 0 };
    AdjustWindowRectEx(&wr, winStyle, TRUE /* has a menu */, 0);
    int windowWidth = wr.right - wr.left;

    HWND hwnd = CreateWindowExA(0, "HyteraHardwarePanelClass", "Hytera Telemetry System Hardware Deck",
                                winStyle | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, 320, NULL, NULL, hInstance, NULL);
    if (!hwnd) return -1;
    ThreadParams* params = (ThreadParams*)malloc(sizeof(ThreadParams));
    if (params) {
        snprintf(params->target_ip, sizeof(params->target_ip), "%s", __argv[1]);
        snprintf(g_repeaterIp, sizeof(g_repeaterIp), "%s", __argv[1]);
        params->hMainWnd = hwnd;
        _beginthreadex(NULL, 0, TelemetryThread, params, 0, NULL);
    }
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    WSACleanup(); return (int)msg.wParam;
}
