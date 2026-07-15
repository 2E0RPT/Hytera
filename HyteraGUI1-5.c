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

typedef enum { TELEM_FLOAT, TELEM_INT, TELEM_ALARM_TRISTATE, TELEM_ALARM_BISTATE, TELEM_STRING } telem_type_t;

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

// Isolated specific OID configuration table entry for the string node
static const telem_oid_t STRING_OID = { "Channel Name", 4, 9, TELEM_STRING, "" };



typedef struct {
    float floats[TELEM_COUNT];
    long ints[TELEM_COUNT];
    int is_float[TELEM_COUNT];
    char channel_name[64];
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

static void build_telem_oid_content(uint8_t* out, uint8_t branch, uint8_t item) {
    static const uint8_t PREFIX[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02, 0x01 };
    memcpy(out, PREFIX, sizeof(PREFIX)); out[sizeof(PREFIX) + 0] = branch; out[sizeof(PREFIX) + 1] = item; out[sizeof(PREFIX) + 2] = 0x00;
}

static int build_channel_name_request(uint8_t* out_buf, int request_id) {
    uint8_t oid_content[14];
    build_telem_oid_content(oid_content, STRING_OID.branch, STRING_OID.item);

    uint8_t oid_tlv[16];
    int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, oid_content, sizeof(oid_content));

    uint8_t null_tlv[2] = { 0x05, 0x00 };
    uint8_t varbind_content[32];
    int vc_len = 0;
    memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
    memcpy(varbind_content + vc_len, null_tlv, sizeof(null_tlv)); vc_len += sizeof(null_tlv);

    uint8_t varbind_list[48];
    int vb_len = ber_append_tlv(varbind_list, 0, 0x30, varbind_content, vc_len);

    uint8_t varbind_list_tlv[64];
    int vbl_len = ber_append_tlv(varbind_list_tlv, 0, 0x30, varbind_list, vb_len);

    uint8_t reqid[3]   = { 0x02, 0x01, (uint8_t)(request_id & 0xFF) };
    uint8_t errstat[3] = { 0x02, 0x01, 0x00 };
    uint8_t errindex[3]= { 0x02, 0x01, 0x00 };

    uint8_t pdu_content[96];
    int pc_len = 0;
    memcpy(pdu_content + pc_len, reqid, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errstat, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errindex, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, varbind_list_tlv, vbl_len); pc_len += vbl_len;

    uint8_t pdu_tlv[112];
    int pdu_len = ber_append_tlv(pdu_tlv, 0, 0xA0, pdu_content, pc_len);

    uint8_t version[3] = { 0x02, 0x01, 0x00 };
    uint8_t community[8] = { 0x04, 0x06, 'p','u','b','l','i','c' };

    uint8_t msg_content[140];
    int mc_len = 0;
    memcpy(msg_content + mc_len, version, 3); mc_len += 3;
    memcpy(msg_content + mc_len, community, 8); mc_len += 8;
    memcpy(msg_content + mc_len, pdu_tlv, pdu_len); mc_len += pdu_len;

    return ber_append_tlv(out_buf, 0, 0x30, msg_content, mc_len);
}

static int parse_telem_getresponse(const uint8_t* buf, int buf_len, TelemetryData* outData) {
    int off = 0; uint8_t tag; const uint8_t* content; int clen;
    if (!ber_read_tlv(buf, buf_len, &off, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* msg = content; int msg_len = clen; int mo = 0;
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0;
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0;
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0;
    if (tag != 0xA2) return 0;
    const uint8_t* pdu = content; int pdu_len = clen; int po = 0;
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0;
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0;
    long error_status = ber_decode_integer(content, clen);
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0;
    if (error_status != 0) return 0;
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* vbl = content; int vbl_len = clen; int vo = 0;

    for (size_t i = 0; i < TELEM_COUNT; i++) {
        if (!ber_read_tlv(vbl, vbl_len, &vo, &tag, &content, &clen) || tag != 0x30) return 0;
        const uint8_t* vb = content; int vb_len = clen; int vbo = 0;
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0;
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0;

        if (tag == 0x04) {
            outData->floats[i] = decode_hytera_float(content, clen);
            outData->is_float[i] = 1;
        } else if (tag == 0x02) {
            outData->ints[i] = ber_decode_integer(content, clen);
            outData->is_float[i] = 0;
        } else { 
            return 0; 
        }

    }
    return 1;

}

// --- Asynchronous Network Communication Engine Thread Loop ---
unsigned __stdcall TelemetryThread(void* pArguments) {
    ThreadParams* params = (ThreadParams*)pArguments;
    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) return 0;

    DWORD snmp_timeout = 1500; // Snappy timeout window handling
    setsockopt(snmp_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&snmp_timeout, sizeof(snmp_timeout));

    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(REPEATER_SNMP_PORT);
    inet_pton(AF_INET, params->target_ip, &snmp_addr.sin_addr);

    int request_id = 1;
    TelemetryData localData;
    memset(&localData, 0, sizeof(localData));

    while (1) {
        // --- Packet Sequence 1: Poll Core Telemetry Gauges ---
        uint8_t req_telem[1100];
        int req_t_len = build_telem_getrequest(req_telem, request_id++);
        sendto(snmp_sock, (const char*)req_telem, req_t_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));

        uint8_t resp_telem[1500];
        struct sockaddr_in from_addr; int from_len = sizeof(from_addr);
        int n_telem = recvfrom(snmp_sock, (char*)resp_telem, sizeof(resp_telem), 0, (struct sockaddr*)&from_addr, &from_len);

        if (n_telem > 0) {
            // Internal static copy parses the 9 core telemetry metrics cleanly
            float floats[9]; long ints[9]; int is_float[9];
            if (parse_telem_getresponse(resp_telem, n_telem, floats, ints, is_float)) {
                memcpy(localData.floats, floats, sizeof(floats));
                memcpy(localData.ints, ints, sizeof(ints));
                memcpy(localData.is_float, is_float, sizeof(is_float));
                localData.is_online = TRUE;
            }
        } else {
            localData.is_online = FALSE;
        }

        // Quick thread sleep boundary spacing between UDP commands
        Sleep(100);

        // --- Packet Sequence 2: Poll Isolated Channel String ---
        uint8_t req_string[140];
        int req_s_len = build_channel_name_request(req_string, request_id++);
        sendto(snmp_sock, (const char*)req_string, req_s_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));

        uint8_t resp_string[1000];
        int n_string = recvfrom(snmp_sock, (char*)resp_string, sizeof(resp_string), 0, (struct sockaddr*)&from_addr, &from_len);

        if (n_string > 0) {
            int off = 0; uint8_t tag; const uint8_t* content; int clen;
            // Parse raw ASN.1 structure bounds down to the OctetString tag (0x04)
            if (ber_read_tlv(resp_string, n_string, &off, &tag, &content, &clen) && tag == 0x30) {
                int mo = 0;
                ber_read_tlv(content, clen, &mo, &tag, &content, &clen); // skip version
                ber_read_tlv(content, clen, &mo, &tag, &content, &clen); // skip community
                ber_read_tlv(content, clen, &mo, &tag, &content, &clen); // unwrap getresponse
                
                int po = 0;
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // skip reqid
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // skip errstat
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // skip errindex
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // unwrap varbind list
                
                int vo = 0;
                ber_read_tlv(content, clen, &vo, &tag, &content, &clen); // unwrap varbind
                int vbo = 0;
                ber_read_tlv(content, clen, &vbo, &tag, &content, &clen); // skip OID
                ber_read_tlv(content, clen, &vbo, &tag, &content, &clen); // final data payload tag
                
                if (tag == 0x04) {
                    int charIdx = 0;
                    // Hop by 2 bytes to step through the UTF-16LE string data bytes cleanly
                    for (int k = 0; k < clen && charIdx < 63; k += 2) {
                        if (content[k] >= 32 && content[k] <= 126) {
                            localData.channel_name[charIdx++] = (char)content[k];
                        }
                    }
                    localData.channel_name[charIdx] = '\0';
                }
            }
        }

        // Safely serialize and transport compiled data structure matrix to Win32 Main UI thread
        TelemetryData* dataPayload = (TelemetryData*)malloc(sizeof(TelemetryData));
        if (dataPayload) {
            *dataPayload = localData;
            PostMessage(params->hMainWnd, WM_TELEMETRY_READY, 0, (LPARAM)dataPayload);
        }

        Sleep(InterlockedExchangeAdd(&g_PollIntervalMs, 0));
    }
    closesocket(snmp_sock);
    free(params);
    return 0;
}
