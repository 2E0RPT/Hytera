#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

// ==========================================================
// HyteraConsole -- standalone repeater telemetry monitor.
//
// Split out from HyteraTransceiver, which originally polled this same
// data inline every 5 seconds: with RX/TX/PTT status also scrolling in
// that same window, a telemetry block landing every 5 seconds made the
// console unreadable. Running it here, in its own window, solves that
// cleanly rather than trying to rate-limit or dial back the display.
//
// Also drops rptFanSpeed, rptFanAlarm, and rptBatteryVoltage -- all
// three are explicitly marked "not supported yet" in Hytera's own
// HYTERA-REPEATER-MIB, so they'd only ever show meaningless static
// values. rptBatteryVoltageAlarm is kept, since the MIB does NOT mark
// that one as unsupported (only the raw battery voltage reading).
// ==========================================================

#define REPEATER_SNMP_PORT 161
#define POLL_INTERVAL_MS   5000

// ------------------------------------------------------------
// Generic BER encode/decode helpers (unchanged from the version
// developed and confirmed working in HyteraTransceiver1-9).
// ------------------------------------------------------------

static int ber_write_length(uint8_t* buf, int offset, int len) {
    if (len < 128) {
        buf[offset] = (uint8_t)len;
        return 1;
    } else if (len < 256) {
        buf[offset] = 0x81;
        buf[offset + 1] = (uint8_t)len;
        return 2;
    } else {
        buf[offset] = 0x82;
        buf[offset + 1] = (uint8_t)(len >> 8);
        buf[offset + 2] = (uint8_t)(len & 0xFF);
        return 3;
    }
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
    } else {
        content_len = len_byte;
    }
    if (content_len < 0 || *offset + content_len > buf_len) return 0;
    *out_tag = tag;
    *out_content = buf + *offset;
    *out_content_len = content_len;
    *offset += content_len;
    return 1;
}

static long ber_decode_integer(const uint8_t* content, int len) {
    long value = (len > 0 && (content[0] & 0x80)) ? -1 : 0;
    for (int i = 0; i < len; i++) value = (value << 8) | content[i];
    return value;
}

// Hytera's OCTET STRING(4) "float format" values are a raw 32-bit
// IEEE-754 float in little-endian byte order -- confirmed by decoding a
// real rptPaTemprature reading (0x00 00 D8 41 -> reversed 41 D8 00 00
// -> 27.0, a plausible PA temperature). Windows on x86/x64 stores
// floats the same way in memory, so no byte-swap is needed here.
static float decode_hytera_float(const uint8_t* content, int len) {
    float f = 0.0f;
    if (len == 4) memcpy(&f, content, 4);
    return f;
}

// ------------------------------------------------------------
// Telemetry OID table -- 9 objects (Fan Speed/Alarm and Battery Voltage
// removed; all confirmed "not supported yet" in HYTERA-REPEATER-MIB).
// OID = .1.3.6.1.4.1.40297.1.2.1.<branch>.<item>.0
//   branch: 1 = rptAlarmInfo, 2 = rptDataInfo
// ------------------------------------------------------------

typedef enum { TELEM_FLOAT, TELEM_INT, TELEM_ALARM_TRISTATE, TELEM_ALARM_BISTATE } telem_type_t;

typedef struct {
    const char* label;
    uint8_t branch;
    uint8_t item;
    telem_type_t type;
    const char* unit;
} telem_oid_t;

static const telem_oid_t TELEM_OIDS[] = {
    { "Voltage",        2, 1,  TELEM_FLOAT,          "V"  }, // 0
    { "Voltage Alarm",  1, 1,  TELEM_ALARM_TRISTATE,  ""  }, // 1
    { "PA Temp",        2, 2,  TELEM_FLOAT,          "C"  }, // 2
    { "Temp Alarm",     1, 2,  TELEM_ALARM_TRISTATE,  ""  }, // 3
    { "VSWR",           2, 4,  TELEM_FLOAT,           ""  }, // 4
    { "VSWR Alarm",     1, 6,  TELEM_ALARM_BISTATE,   ""  }, // 5
    { "Battery Alarm",  1, 9,  TELEM_ALARM_BISTATE,   ""  }, // 6
    { "Slot1 RSSI",     2, 9,  TELEM_INT,             "dB" }, // 7
    { "Slot2 RSSI",     2, 10, TELEM_INT,             "dB" }, // 8
};
#define TELEM_COUNT (sizeof(TELEM_OIDS) / sizeof(TELEM_OIDS[0]))

static void build_telem_oid_content(uint8_t* out, uint8_t branch, uint8_t item) {
    static const uint8_t PREFIX[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02, 0x01 };
    memcpy(out, PREFIX, sizeof(PREFIX));
    out[sizeof(PREFIX) + 0] = branch;
    out[sizeof(PREFIX) + 1] = item;
    out[sizeof(PREFIX) + 2] = 0x00; // scalar instance
}

static int build_telem_getrequest(uint8_t* out_buf, int request_id) {
    uint8_t varbind_list[1024];
    int vb_offset = 0;

    for (size_t i = 0; i < TELEM_COUNT; i++) {
        uint8_t oid_content[14];
        build_telem_oid_content(oid_content, TELEM_OIDS[i].branch, TELEM_OIDS[i].item);

        uint8_t oid_tlv[16];
        int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, oid_content, sizeof(oid_content));

        uint8_t null_tlv[2] = { 0x05, 0x00 };

        uint8_t varbind_content[32];
        int vc_len = 0;
        memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
        memcpy(varbind_content + vc_len, null_tlv, sizeof(null_tlv)); vc_len += sizeof(null_tlv);

        vb_offset = ber_append_tlv(varbind_list, vb_offset, 0x30, varbind_content, vc_len);
    }

    uint8_t varbind_list_tlv[1040];
    int vbl_len = ber_append_tlv(varbind_list_tlv, 0, 0x30, varbind_list, vb_offset);

    uint8_t reqid[3]   = { 0x02, 0x01, (uint8_t)(request_id & 0xFF) };
    uint8_t errstat[3] = { 0x02, 0x01, 0x00 };
    uint8_t errindex[3]= { 0x02, 0x01, 0x00 };

    uint8_t pdu_content[1060];
    int pc_len = 0;
    memcpy(pdu_content + pc_len, reqid, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errstat, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errindex, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, varbind_list_tlv, vbl_len); pc_len += vbl_len;

    uint8_t pdu_tlv[1070];
    int pdu_len = ber_append_tlv(pdu_tlv, 0, 0xA0, pdu_content, pc_len); // GetRequest-PDU

    uint8_t version[3] = { 0x02, 0x01, 0x00 };
    uint8_t community[8] = { 0x04, 0x06, 'p','u','b','l','i','c' };

    uint8_t msg_content[1090];
    int mc_len = 0;
    memcpy(msg_content + mc_len, version, 3); mc_len += 3;
    memcpy(msg_content + mc_len, community, 8); mc_len += 8;
    memcpy(msg_content + mc_len, pdu_tlv, pdu_len); mc_len += pdu_len;

    return ber_append_tlv(out_buf, 0, 0x30, msg_content, mc_len);
}

static int parse_telem_getresponse(const uint8_t* buf, int buf_len, float* out_floats, long* out_ints, int* out_is_float) {
    int off = 0;
    uint8_t tag; const uint8_t* content; int clen;

    if (!ber_read_tlv(buf, buf_len, &off, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* msg = content; int msg_len = clen; int mo = 0;

    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // version
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // community

    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0;
    if (tag != 0xA2) return 0; // not a GetResponse
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

        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // OID -- trust ordering
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // value

        if (tag == 0x04) {
            out_floats[i] = decode_hytera_float(content, clen);
            out_is_float[i] = 1;
        } else if (tag == 0x02) {
            out_ints[i] = ber_decode_integer(content, clen);
            out_is_float[i] = 0;
        } else {
            return 0;
        }
    }
    return 1;
}

// ------------------------------------------------------------
// Display
// ------------------------------------------------------------

static HANDLE g_hConsoleOut = NULL;
static WORD   g_defaultConsoleAttributes = 0;
#define COLOR_ALARM (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY) // bright yellow

static void print_telem_alarm_word(long value, telem_type_t type) {
    const char* word;
    int is_alarm;
    if (type == TELEM_ALARM_TRISTATE) {
        word = (value == 0) ? "OK" : (value == 1) ? "LOW" : (value == 2) ? "HIGH" : "?";
        is_alarm = (value != 0);
    } else {
        word = (value == 0) ? "OK" : "ALARM";
        is_alarm = (value != 0);
    }
    if (is_alarm) SetConsoleTextAttribute(g_hConsoleOut, COLOR_ALARM);
    printf("[%s]", word);
    if (is_alarm) SetConsoleTextAttribute(g_hConsoleOut, g_defaultConsoleAttributes);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("HyteraConsole 1.0 -- repeater telemetry monitor\n");
        printf("Usage: %s <repeater_ip>\n", argv[0]);
        printf("Example: %s 192.168.1.167\n", argv[0]);
        return -1;
    }

    struct in_addr testAddr;
    if (inet_pton(AF_INET, argv[1], &testAddr) != 1) {
        printf("[ERROR] '%s' is not a valid IPv4 address.\n", argv[1]);
        return -1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock initialization failed.\n");
        return -1;
    }

    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) {
        printf("[ERROR] Could not create SNMP socket.\n");
        WSACleanup();
        return -1;
    }
    DWORD snmp_timeout = 2000;
    setsockopt(snmp_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&snmp_timeout, sizeof(snmp_timeout));

    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(REPEATER_SNMP_PORT);
    inet_pton(AF_INET, argv[1], &snmp_addr.sin_addr);

    g_hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_hConsoleOut, &csbi)) {
        g_defaultConsoleAttributes = csbi.wAttributes;
    } else {
        g_defaultConsoleAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    printf("HyteraConsole 1.0 -- monitoring repeater at %s\n", argv[1]);
    printf("Polling every %d seconds. Press ESC to quit.\n\n", POLL_INTERVAL_MS / 1000);

    int request_id = 1;

    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            printf("\n[SYSTEM] Exiting.\n");
            break;
        }

        uint8_t request[1100];
        int req_len = build_telem_getrequest(request, request_id++);
        sendto(snmp_sock, (const char*)request, req_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));

        uint8_t response[1500];
        struct sockaddr_in from_addr; int from_len = sizeof(from_addr);
        int n = recvfrom(snmp_sock, (char*)response, sizeof(response), 0, (struct sockaddr*)&from_addr, &from_len);

        if (n > 0) {
            float floats[TELEM_COUNT]; long ints[TELEM_COUNT]; int is_float[TELEM_COUNT];
            if (parse_telem_getresponse(response, n, floats, ints, is_float)) {
                printf("--- Repeater Telemetry ---\n");

                printf("  Voltage:  %5.2f %-2s ", floats[0], TELEM_OIDS[0].unit);
                print_telem_alarm_word(ints[1], TELEM_ALARM_TRISTATE);
                printf("   PA Temp: %5.1f %-2s ", floats[2], TELEM_OIDS[2].unit);
                print_telem_alarm_word(ints[3], TELEM_ALARM_TRISTATE);
                printf("\n");

                printf("  VSWR: %5.2f ", floats[4]);
                print_telem_alarm_word(ints[5], TELEM_ALARM_BISTATE);
                printf("   Battery: ");
                print_telem_alarm_word(ints[6], TELEM_ALARM_BISTATE);
                printf("\n");

                printf("  Slot1 RSSI: %ld dB   Slot2 RSSI: %ld dB\n", ints[7], ints[8]);
                printf("--------------------------\n\n");
                fflush(stdout);
            } else {
                printf("[SNMP] Telemetry response could not be parsed (unexpected format or error status).\n\n");
            }
        } else {
            printf("[SNMP] No telemetry response from repeater (timeout).\n\n");
        }

        for (int waited = 0; waited < POLL_INTERVAL_MS; waited += 100) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;
            Sleep(100);
        }
    }

    closesocket(snmp_sock);
    WSACleanup();
    return 0;
}
