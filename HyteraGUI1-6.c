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
    { "Voltage",        2, 1,  TELEM_FLOAT,          "V"  },  // 0
    { "Voltage Alarm",  1, 1,  TELEM_ALARM_TRISTATE,  ""  },  // 1
    { "PA Temp",        2, 2,  TELEM_FLOAT,          "C"  },  // 2
    { "Temp Alarm",     1, 2,  TELEM_ALARM_TRISTATE,  ""  },  // 3
    { "VSWR",           2, 4,  TELEM_FLOAT,           ""  },  // 4
    { "VSWR Alarm",     1, 6,  TELEM_ALARM_BISTATE,   ""  },  // 5
    { "Battery Alarm",  1, 9,  TELEM_ALARM_BISTATE,   ""  },  // 6
    { "Slot1 RSSI",     2, 9,  TELEM_INT,             "dB" }, // 7
    { "Slot2 RSSI",     2, 10, TELEM_INT,             "dB" }  // 8
};
#define TELEM_COUNT (sizeof(TELEM_OIDS) / sizeof(TELEM_OIDS[0]))

// Isolated branch path parameter explicitly verified by the iReasoning MIB Browser layout
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

static int build_telem_getrequest(uint8_t* out_buf, int request_id) {
    uint8_t varbind_list[1024]; int vb_offset = 0;
    for (size_t i = 0; i < TELEM_COUNT; i++) {
        uint8_t oid_content[14]; build_telem_oid_content(oid_content, TELEM_OIDS[i].branch, TELEM_OIDS[i].item);
        uint8_t oid_tlv[16]; int oid_len = ber_append_tlv(oid_tlv, 0, 0x06, oid_content, sizeof(oid_content));
        uint8_t null_tlv[2] = { 0x05, 0x00 };
        uint8_t varbind_content[32]; int vc_len = 0;
        memcpy(varbind_content, oid_tlv, oid_len); vc_len += oid_len;
        memcpy(varbind_content + vc_len, null_tlv, sizeof(null_tlv)); vc_len += sizeof(null_tlv);
        vb_offset = ber_append_tlv(varbind_list, vb_offset, 0x30, varbind_content, vc_len);
    }
    uint8_t varbind_list_tlv[1040]; int vbl_len = ber_append_tlv(varbind_list_tlv, 0, 0x30, varbind_list, vb_offset);
    uint8_t reqid[3] = { 0x02, 0x01, (uint8_t)(request_id & 0xFF) };
    uint8_t errstat[3] = { 0x02, 0x01, 0x00 }; uint8_t errindex[3] = { 0x02, 0x01, 0x00 };
    uint8_t pdu_content[1060]; int pc_len = 0;
    memcpy(pdu_content + pc_len, reqid, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errstat, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, errindex, 3); pc_len += 3;
    memcpy(pdu_content + pc_len, varbind_list_tlv, vbl_len); pc_len += vbl_len;
    uint8_t pdu_tlv[1070]; int pdu_len = ber_append_tlv(pdu_tlv, 0, 0xA0, pdu_content, pc_len);
    uint8_t version[3] = { 0x02, 0x01, 0x00 }; uint8_t community[8] = { 0x04, 0x06, 'p','u','b','l','i','c' };
    uint8_t msg_content[1090]; int mc_len = 0;
    memcpy(msg_content + mc_len, version, 3); mc_len += 3;
    memcpy(msg_content + mc_len, community, 8); mc_len += 8;
    memcpy(msg_content + mc_len, pdu_tlv, pdu_len); mc_len += pdu_len;
    return ber_append_tlv(out_buf, 0, 0x30, msg_content, mc_len);
}

static int build_channel_name_request(uint8_t* out_buf, int request_id) {
    // Exact 90-byte packet frame definition matching the working Wireshark get-request capture
    uint8_t raw_snmp_frame[] = {
        0x30, 0x2e,                                           // Sequence wrapper (Length 46)
        0x02, 0x01, 0x00,                                     // Version: SNMP v1 (0)
        0x04, 0x06, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63,       // Community String: "public"
        0xa0, 0x21,                                           // GetRequest-PDU Type Tag (Length 33)
        0x02, 0x01, (uint8_t)(request_id & 0xFF),             // Dynamic Request ID
        0x02, 0x01, 0x00,                                     // Error Status: 0
        0x02, 0x01, 0x00,                                     // Error Index: 0
        0x30, 0x16,                                           // VarBind List Array Wrapper (Length 22)
        0x30, 0x14,                                           // Individual VarBind Object (Length 20)
        0x06, 0x10,                                           // Object Identifier (OID) Tag (Length 16)
        // Raw sub-identifier byte stream for path .1.3.6.1.4.1.40297.1.2.4.9.0
        0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xba, 0x69, 0x01, 0x02, 0x04, 0x09, 0x00,
        0x05, 0x00                                            // ASN.1 Null Value Data Attachment
    };

    memcpy(out_buf, raw_snmp_frame, sizeof(raw_snmp_frame));
    return (int)sizeof(raw_snmp_frame);
}


static int parse_telem_getresponse(const uint8_t* buf, int buf_len, float* out_floats, long* out_ints, int* out_is_float) {
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
            out_floats[i] = decode_hytera_float(content, clen); out_is_float[i] = 1;
        } else if (tag == 0x02) {
            out_ints[i] = ber_decode_integer(content, clen); out_is_float[i] = 0;
        } else { return 0; }
    }
    return 1;
}

// --- Asynchronous Network Thread Loop ---
unsigned __stdcall TelemetryThread(void* pArguments) {
    ThreadParams* params = (ThreadParams*)pArguments;
    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) return 0;

        DWORD snmp_timeout = 1500;
    setsockopt(snmp_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&snmp_timeout, sizeof(snmp_timeout));

    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(REPEATER_SNMP_PORT);
    inet_pton(AF_INET, params->target_ip, &snmp_addr.sin_addr);

    int request_id = 1;
    TelemetryData localData;
    memset(&localData, 0, sizeof(localData));
    strcpy(localData.channel_name, "STANDBY");

    while (1) {
        // 1. Fetch Dials Telemetry Array Block
        uint8_t req_telem[1100];
        int req_t_len = build_telem_getrequest(req_telem, request_id++);
        sendto(snmp_sock, (const char*)req_telem, req_t_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));

        uint8_t resp_telem[1500];
        struct sockaddr_in from_addr; int from_len = sizeof(from_addr);
        int n_telem = recvfrom(snmp_sock, (char*)resp_telem, sizeof(resp_telem), 0, (struct sockaddr*)&from_addr, &from_len);

        if (n_telem > 0) {
            float floats[TELEM_COUNT]; long ints[TELEM_COUNT]; int is_float[TELEM_COUNT];
            if (parse_telem_getresponse(resp_telem, n_telem, floats, ints, is_float)) {
                memcpy(localData.floats, floats, sizeof(floats));
                memcpy(localData.ints, ints, sizeof(ints));
                memcpy(localData.is_float, is_float, sizeof(is_float));
                localData.is_online = TRUE;
            }
        } else {
            localData.is_online = FALSE;
        }

        Sleep(100);

        // 2. Fetch Separated Text OID Branch Channel Description (Using the iReasoning verified 4.9 path)
        uint8_t req_string[1100];
        int req_s_len = build_channel_name_request(req_string, request_id++);
        sendto(snmp_sock, (const char*)req_string, req_s_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));

        uint8_t resp_string[1500];
        int n_string = recvfrom(snmp_sock, (char*)resp_string, sizeof(resp_string), 0, (struct sockaddr*)&from_addr, &from_len);

        if (n_string > 0) {
            int off = 0; uint8_t tag; const uint8_t* content; int clen;
            if (ber_read_tlv(resp_string, n_string, &off, &tag, &content, &clen) && tag == 0x30) {
                int mo = 0;
                ber_read_tlv(content, clen, &mo, &tag, &content, &clen); // version
                ber_read_tlv(content, clen, &mo, &tag, &content, &clen); // community
                ber_read_tlv(content, clen, &mo, &tag, &content, &clen); // unwrap response
                
                int po = 0;
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // reqid
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // skip errstat
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // skip errindex
                ber_read_tlv(content, clen, &po, &tag, &content, &clen); // unwrap varbind list
                
                int vo = 0;
                ber_read_tlv(content, clen, &vo, &tag, &content, &clen); // unwrap varbind
                int vbo = 0;
                ber_read_tlv(content, clen, &vbo, &tag, &content, &clen); // skip OID
                ber_read_tlv(content, clen, &vbo, &tag, &content, &clen); // character content block
                
                if (tag == 0x04) {
                    int charIdx = 0;
                    for (int k = 0; k < clen && charIdx < 63; k += 2) {
                        if (content[k] >= 32 && content[k] <= 126) {
                            localData.channel_name[charIdx++] = (char)content[k];
                        }
                    }
                    localData.channel_name[charIdx] = '\0';
                }
            }
        }


        TelemetryData* dataPayload = (TelemetryData*)malloc(sizeof(TelemetryData));
        if (dataPayload) {
            *dataPayload = localData;
            PostMessage(params->hMainWnd, WM_TELEMETRY_READY, 0, (LPARAM)dataPayload);
        }
        Sleep(InterlockedExchangeAdd(&g_PollIntervalMs, 0));
    }
    closesocket(snmp_sock); free(params); return 0;
}

// --- Gauge Painting Helper System ---
void PaintAnalogGauge(HDC hdc, int x, int y, int w, int h, const char* name, float value, float minVal, float maxVal, BOOL isAlarm, const char* fmt) {
    HBRUSH bgBrush = CreateSolidBrush(isAlarm ? RGB(140, 20, 20) : RGB(228, 224, 210));
    RECT rc = { x, y, x + w, y + h }; FillRect(hdc, &rc, bgBrush); DrawEdge(hdc, &rc, EDGE_SUNKEN, BF_RECT); DeleteObject(bgBrush);

    HPEN arcPen = CreatePen(PS_SOLID, 2, isAlarm ? RGB(230, 230, 230) : RGB(40, 40, 40));
    HGDIOBJ oldPen = SelectObject(hdc, arcPen);
    int cx = x + w / 2; int cy = y + h - 22; int radius = w / 2 - 18;
    Arc(hdc, cx - radius, cy - radius, cx + radius, cy + radius, cx + radius, cy, cx - radius, cy);

    float pct = (value - minVal) / (maxVal - minVal);
    if (pct < 0.0f) pct = 0.0f; if (pct > 1.0f) pct = 1.0f;
    float angle_rad = 3.14159265f * (1.0f - pct);
    int nx = cx + (int)(cosf(angle_rad) * (radius - 4)); int ny = cy - (int)(sinf(angle_rad) * (radius - 4));

    HPEN needlePen = CreatePen(PS_SOLID, 2, isAlarm ? RGB(255, 255, 0) : RGB(220, 20, 20));
    SelectObject(hdc, needlePen); MoveToEx(hdc, cx, cy, NULL); LineTo(hdc, nx, ny);

    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, isAlarm ? RGB(255, 255, 255) : RGB(40, 40, 40));
    HFONT hMedFont = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    HGDIOBJ oldFont = SelectObject(hdc, hMedFont); UINT oldAlign = SetTextAlign(hdc, TA_CENTER);
    TextOutA(hdc, cx, y + h - 17, name, (int)strlen(name));

    char lblMin[16], lblMid[16], lblMax[16]; float midVal = minVal + (maxVal - minVal) / 2.0f;
    if (minVal == (int)minVal && maxVal == (int)maxVal) {
        snprintf(lblMin, sizeof(lblMin), "%d", (int)minVal); snprintf(lblMid, sizeof(lblMid), "%d", (int)midVal); snprintf(lblMax, sizeof(lblMax), "%d", (int)maxVal);
    } else {
        snprintf(lblMin, sizeof(lblMin), "%.1f", minVal); snprintf(lblMid, sizeof(lblMid), "%.1f", midVal); snprintf(lblMax, sizeof(lblMax), "%.1f", maxVal);
    }
    TextOutA(hdc, cx - radius + 8, cy - 14, lblMin, (int)strlen(lblMin));
    TextOutA(hdc, cx, cy - radius - 12, lblMid, (int)strlen(lblMid));
    TextOutA(hdc, cx + radius - 8, cy - 14, lblMax, (int)strlen(lblMax));

    HFONT hValFont = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial");
    SelectObject(hdc, hValFont); SetTextColor(hdc, isAlarm ? RGB(255, 255, 0) : RGB(0, 102, 204));
    
    char valStr[32];
    if (strcmp(fmt, "IDLE") == 0) { strcpy(valStr, "IDLE"); }
    else { snprintf(valStr, sizeof(valStr), fmt, value); }
    TextOutA(hdc, cx, cy - 16, valStr, (int)strlen(valStr));

    SetTextAlign(hdc, oldAlign); SelectObject(hdc, oldFont); DeleteObject(hMedFont); DeleteObject(hValFont);
    SelectObject(hdc, oldPen); DeleteObject(arcPen); DeleteObject(needlePen);
}

// --- Main Window Event Callback ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static TelemetryData currentTelem; static BOOL receivedFirstData = FALSE;
    switch (msg) {
        case WM_CREATE: {
            CreateWindowA("BUTTON", "PTT", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,  40,  210, 80, 45, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "TS1", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,  160, 210, 60, 45, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "TS2", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,  240, 210, 60, 45, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "DMR", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,  320, 210, 60, 45, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "FM",  WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,  400, 210, 60, 45, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "TEMP", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 820, 210, 80, 45, hwnd, (HMENU)5001, NULL, NULL);

            HMENU hMenuBar = CreateMenu(); HMENU hSubMenu = CreatePopupMenu();
            for (int i = 1; i <= 50; i++) {
                char menuItemText[32]; snprintf(menuItemText, sizeof(menuItemText), "%.1f Seconds", i / 10.0f);
                AppendMenuA(hSubMenu, MF_STRING, 2000 + i, menuItemText);
            }
            AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hSubMenu, "Update"); SetMenu(hwnd, hMenuBar); break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId > 2000 && wmId <= 2050) {
                LONG targetMs = (wmId - 2000) * 100; InterlockedExchange(&g_PollIntervalMs, targetMs);
            } else if (wmId == 5001) {
                MessageBoxA(hwnd, currentTelem.channel_name, "Decoded String Test", MB_OK);
            }
            break;
        }
        case WM_TELEMETRY_READY: {
            TelemetryData* incoming = (TelemetryData*)lParam;
            if (incoming) { currentTelem = *incoming; receivedFirstData = TRUE; free(incoming); InvalidateRect(hwnd, NULL, TRUE); }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            HBRUSH hChassisBrush = CreateSolidBrush(RGB(28, 28, 30)); RECT clientRc; GetClientRect(hwnd, &clientRc); FillRect(hdc, &clientRc, hChassisBrush); DeleteObject(hChassisBrush);
            HBRUSH hLedBg = CreateSolidBrush(RGB(15, 15, 15)); RECT rcLedL = { 30, 20, 450, 75 }; FillRect(hdc, &rcLedL, hLedBg); DrawEdge(hdc, &rcLedL, EDGE_SUNKEN, BF_RECT);
            RECT rcLedR = { 480, 20, 900, 75 }; FillRect(hdc, &rcLedR, hLedBg); DrawEdge(hdc, &rcLedR, EDGE_SUNKEN, BF_RECT); DeleteObject(hLedBg);

                        SetBkMode(hdc, TRANSPARENT);
            HFONT hLedFont = CreateFontA(38, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Courier New");
            HGDIOBJ oldFont = SelectObject(hdc, hLedFont);

            if (receivedFirstData && currentTelem.is_online) {
                char strL[64]; snprintf(strL, sizeof(strL), "S1:%3ld  S2:%3ld", currentTelem.ints[7], currentTelem.ints[8]);
                SetTextColor(hdc, RGB(240, 60, 60)); TextOutA(hdc, 50, 28, strL, (int)strlen(strL));
                SetTextColor(hdc, RGB(60, 240, 60)); TextOutA(hdc, 500, 28, currentTelem.channel_name, (int)strlen(currentTelem.channel_name));
            } else {
                SetTextColor(hdc, RGB(60, 20, 20)); TextOutA(hdc, 50, 28, "S1:---  S2:---", 14);
                SetTextColor(hdc, RGB(240, 40, 40)); TextOutA(hdc, 500, 28, "LINK OFFLINE", 12);
            }

            float s1_rssi = receivedFirstData ? (float)currentTelem.ints[7] : -120.0f;
            float s2_rssi = receivedFirstData ? (float)currentTelem.ints[8] : -120.0f;
            float vswr_val = receivedFirstData ? currentTelem.floats[4] : 1.0f;
            float volts_val = receivedFirstData ? currentTelem.floats[0] : 0.0f;
            float temp_val = receivedFirstData ? currentTelem.floats[2] : -20.0f;

            BOOL v_alarm = receivedFirstData ? (currentTelem.ints[1] != 0) : FALSE;
            BOOL t_alarm = receivedFirstData ? (currentTelem.ints[3] != 0) : FALSE;
            BOOL vswr_alarm = receivedFirstData ? (currentTelem.ints[5] != 0) : FALSE;

            const char* fmt1 = (s1_rssi <= -200.0f) ? "IDLE" : "%.0f"; float draw_s1 = (s1_rssi <= -200.0f) ? -120.0f : s1_rssi;
            const char* fmt2 = (s2_rssi <= -200.0f) ? "IDLE" : "%.0f"; float draw_s2 = (s2_rssi <= -200.0f) ? -120.0f : s2_rssi;

            PaintAnalogGauge(hdc, 30,  100, 150, 90, "TS-1 db", draw_s1, -120.0f, -40.0f, FALSE, fmt1);
            PaintAnalogGauge(hdc, 210, 100, 150, 90, "TS-2 db", draw_s2, -120.0f, -40.0f, FALSE, fmt2);
            PaintAnalogGauge(hdc, 390, 100, 150, 90, "VSWR", vswr_val, 1.0f, 5.0f, vswr_alarm, "%.2f");
            PaintAnalogGauge(hdc, 570, 100, 150, 90, "Volts", volts_val, 9.0f, 16.0f, v_alarm, "%.1fV");
            PaintAnalogGauge(hdc, 750, 100, 150, 90, "Temp C", temp_val, -20.0f, 120.0f, t_alarm, "%.1fC");

            SelectObject(hdc, oldFont); DeleteObject(hLedFont); EndPaint(hwnd, &ps); break;
        }
        case WM_DESTROY: { PostQuitMessage(0); break; }
        default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (__argc < 2) { MessageBoxA(NULL, "Usage: hytera_gui.exe <repeater_ip>\nExample: hytera_gui.exe 192.168.1.167", "Missing Parameter", MB_OK); return -1; }
    WSADATA wsaData; if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return -1;
    WNDCLASSEXA wc = {0}; wc.cbSize = sizeof(WNDCLASSEXA); wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.lpszClassName = "HyteraHardwarePanelClass"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassExA(&wc)) return -1;
    HWND hwnd = CreateWindowExA(0, "HyteraHardwarePanelClass", "Hytera Telemetry System Hardware Deck", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 950, 320, NULL, NULL, hInstance, NULL);
    if (!hwnd) return -1;
    ThreadParams* params = (ThreadParams*)malloc(sizeof(ThreadParams));
    if (params) { snprintf(params->target_ip, sizeof(params->target_ip), "%s", __argv[1]); params->hMainWnd = hwnd; _beginthreadex(NULL, 0, TelemetryThread, params, 0, NULL); }
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    WSACleanup(); return (int)msg.wParam;
}


