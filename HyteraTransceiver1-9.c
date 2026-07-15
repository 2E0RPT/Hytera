#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Enable Winsock2 specifications for Windows Vista/7/10+
#endif
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ==========================================================
// SHARED CONFIGURATION -- both RX and TX talk to the same
// repeater on Time Slot 1 (RCP 30009 / RTP 30012).
// ==========================================================
#define LOCAL_IP             "192.168.1.136" // Your PC IP
// REPEATER_IP is no longer a fixed constant -- pass it as a command-line
// argument (e.g. `HyteraTransceiver 192.168.1.167`), or omit it and the
// program will wait for the repeater to contact us first and learn its
// address from the source of that packet, same as the original RX program did.
#define PORT_RCP              30009          // Radio Control Port  (TX only)
#define PORT_RTP              30012          // Voice/RTP Port      (shared RX + TX)

#define TARGET_TALKGROUP      1               // Talkgroup 1
#define CALL_TYPE_GROUP       1               // Group Call

const uint8_t WAKE_CALL_PAYLOAD[]   = {0x32, 0x42, 0x00, 0x05, 0x00, 0x00};
const uint8_t KEEP_ALIVE_PAYLOAD[]  = {0x32, 0x42, 0x00, 0x02, 0x00, 0x00};

const uint8_t CALL_SETUP_TEMPLATE[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x41, 0x08, 0x05, 0x00, 0x01,
    0x7C, 0x09, 0x00, 0x00, 0x5E, 0x03
};

const uint8_t PTT_TEMPLATE[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x41, 0x00, 0x02, 0x00, 0x03,
    0x00, 0x00, 0x03
};

// A single lock so the RX VU-meter thread and the main PTT-status thread
// don't garble each other's console output when they print at the same time.
static CRITICAL_SECTION g_consoleLock;
static void safe_printf(const char* fmt, ...) {
    EnterCriticalSection(&g_consoleLock);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    LeaveCriticalSection(&g_consoleLock);
}

static volatile LONG g_app_running = 1;   // Cleared on Esc to shut everything down
static volatile LONG g_transmitting = 0;  // 1 while spacebar is held (PTT keyed)

// Console coloring: RX meter is light green, TX meter is light red.
static HANDLE g_hConsoleOut = NULL;
static WORD   g_defaultConsoleAttributes = 0;
#define COLOR_RX (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_TX (FOREGROUND_RED   | FOREGROUND_INTENSITY)
#define COLOR_ALARM (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY) // bright yellow -- distinct from RX/TX so it doesn't blend with either VU meter

// TX mic volume, adjusted with LEFT (down) / RIGHT (up) -- same 0.5 step,
// same 0.0-5.0 clamp, same edge-triggered key behavior as RX's Up/Down.
static volatile float g_tx_volume_multiplier = 1.0f;

// ==========================================================
// RX SIDE (from Hytera_TS1_10.c) -- decode incoming u-law audio to speaker
// ==========================================================
#define RX_BUFFER_COUNT   4      // Ring buffers for playback queuing
#define PAYLOAD_OFFSET    29     // NOTE: this is 1 byte more than the TX header below (28 bytes).
                                  // Left exactly as in your working RX code -- see chat notes.
#define NETWORK_BUF_SZ    2048   // Max expected UDP packet size
#define VU_METER_WIDTH    60

static HWAVEOUT hWaveOut = NULL;
static WAVEHDR rxWaveHeaders[RX_BUFFER_COUNT];
static int rxCurrentBufferIndex = 0;
static int16_t ulaw_to_pcm_lut[256];

void init_ulaw_lut(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t data = ~i;
        int sign = (data & 0x80);
        int exponent = (data >> 4) & 0x07;
        int mantissa = data & 0x0F;
        int sample = ((mantissa << 3) + 33) << exponent;
        sample -= 33;
        ulaw_to_pcm_lut[i] = (int16_t)(sign ? -sample : sample);
    }
}

// Standard G.711 linear-PCM-to-u-law encoder (the inverse of the LUT above).
// Needed because TX gain has to be applied in the linear domain -- u-law is
// logarithmic, so scaling the raw byte directly would distort the audio.
#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635
uint8_t linear_to_ulaw(int16_t pcm_val) {
    int sign = (pcm_val >> 8) & 0x80;
    if (sign) pcm_val = (int16_t)(-pcm_val);
    if (pcm_val > ULAW_CLIP) pcm_val = ULAW_CLIP;
    pcm_val += ULAW_BIAS;

    int exponent = 7;
    for (int expMask = 0x4000; (pcm_val & expMask) == 0 && exponent > 0; expMask >>= 1, exponent--);

    int mantissa = (pcm_val >> (exponent + 3)) & 0x0F;
    uint8_t ulawByte = (uint8_t)~(sign | (exponent << 4) | mantissa);
    return ulawByte;
}

int init_playback_device(int sample_rate) {
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[ERROR] Failed to open playback (WAVE_MAPPER) device.\n");
        return 0;
    }

    for (int i = 0; i < RX_BUFFER_COUNT; i++) {
        rxWaveHeaders[i].dwBufferLength = NETWORK_BUF_SZ * 2;
        rxWaveHeaders[i].lpData = (char *)malloc(rxWaveHeaders[i].dwBufferLength);
        rxWaveHeaders[i].dwFlags = 0;
        waveOutPrepareHeader(hWaveOut, &rxWaveHeaders[i], sizeof(WAVEHDR));
        rxWaveHeaders[i].dwFlags |= WHDR_DONE;
    }
    return 1;
}

void shutdown_playback_device(void) {
    if (!hWaveOut) return;
    for (int i = 0; i < RX_BUFFER_COUNT; i++) {
        waveOutUnprepareHeader(hWaveOut, &rxWaveHeaders[i], sizeof(WAVEHDR));
        free(rxWaveHeaders[i].lpData);
    }
    waveOutClose(hWaveOut);
    hWaveOut = NULL;
}

// RX loop -- runs on its own thread. Listens on the shared RTP socket,
// decodes u-law to 16-bit PCM, plays it, and renders the VU meter.
// While g_transmitting is set, incoming audio is still parsed (so the
// "Radio ID talking" line stays accurate) but NOT played to the
// speaker -- half-duplex, avoids hearing your own looped-back audio.
DWORD WINAPI RXThreadFunc(LPVOID lpParam) {
    SOCKET rtp_sock = *(SOCKET*)lpParam;
    char net_buffer[NETWORK_BUF_SZ];
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    uint32_t current_radio_id = 0;
    int is_first_radio_id_parsed = 0;

    int audio_packet_counter = 0;
    double accumulated_amplitude_sum = 0.0;
    long long total_samples_accumulated = 0;
    DWORD last_audio_packet_time = 0;
    int is_currently_receiving_stream = 0;

    float volume_multiplier = 1.0f;

    while (InterlockedCompareExchange(&g_app_running, 0, 0)) {
        // --- Live Keyboard Hotkey Scan Section (Up/Down = volume) ---
        if (GetForegroundWindow() == GetConsoleWindow()) {
            if (GetAsyncKeyState(VK_UP) & 0x0001) {
                volume_multiplier += 0.5f;
                if (volume_multiplier > 5.0f) volume_multiplier = 5.0f;
                safe_printf("\r[Volume Set]: %3.0f%%                                                      \n", volume_multiplier * 100.0f);
            }
            if (GetAsyncKeyState(VK_DOWN) & 0x0001) {
                volume_multiplier -= 0.5f;
                if (volume_multiplier < 0.0f) volume_multiplier = 0.0f;
                safe_printf("\r[Volume Set]: %3.0f%%                                                      \n", volume_multiplier * 100.0f);
            }
        }

        client_addr_len = sizeof(client_addr);
        int bytes_received = recvfrom(rtp_sock, net_buffer, NETWORK_BUF_SZ, 0,
                                      (struct sockaddr *)&client_addr, &client_addr_len);

        DWORD current_time = GetTickCount();

        if (is_currently_receiving_stream && (current_time - last_audio_packet_time >= 2000)) {
            safe_printf("\rAudio Stream Idle.                                                                       \n");
            is_currently_receiving_stream = 0;
            is_first_radio_id_parsed = 0;
        }

        if (bytes_received <= 0) {
            continue; // Socket timeout -- loop back to re-check g_app_running
        }

        if (bytes_received >= 23) {
            uint8_t *ubuf = (uint8_t *)net_buffer;
            uint32_t parsed_id = ((uint32_t)ubuf[17] << 16) |
                                 ((uint32_t)ubuf[18] << 8)  |
                                  (uint32_t)ubuf[19];
            uint32_t parsed_group = ((uint32_t)ubuf[20] << 16) |
                                   ((uint32_t)ubuf[21] << 8)  |
                                    (uint32_t)ubuf[22];

            if (!is_first_radio_id_parsed || parsed_id != current_radio_id) {
                current_radio_id = parsed_id;
                is_first_radio_id_parsed = 1;
                if (is_currently_receiving_stream) safe_printf("\n");
                safe_printf("Radio ID: %u talking to Group %u\n", current_radio_id, parsed_group);
                is_currently_receiving_stream = 1;
            }
        }

        if (bytes_received <= PAYLOAD_OFFSET) continue;

        int audio_payload_len = bytes_received - PAYLOAD_OFFSET;
        uint8_t *ulaw_ptr = (uint8_t *)(net_buffer + PAYLOAD_OFFSET);

        last_audio_packet_time = GetTickCount();
        is_currently_receiving_stream = 1;
        audio_packet_counter++;

        // Skip actual playback while transmitting -- half-duplex.
        // Still counted above so the VU meter / idle detection stay in sync.
        int should_play = !InterlockedCompareExchange(&g_transmitting, 0, 0);

        WAVEHDR *hdr = &rxWaveHeaders[rxCurrentBufferIndex];
        if (should_play) {
            while (!(hdr->dwFlags & WHDR_DONE)) {
                Sleep(1);
            }
        }

        int16_t *pcm_out = (int16_t *)hdr->lpData;
        for (int i = 0; i < audio_payload_len; i++) {
            int16_t sample = ulaw_to_pcm_lut[ulaw_ptr[i]];
            float amplified_sample = (float)sample * volume_multiplier;
            if (amplified_sample > 32767.0f) sample = 32767;
            else if (amplified_sample < -32768.0f) sample = -32768;
            else sample = (int16_t)amplified_sample;

            if (should_play) pcm_out[i] = sample;

            accumulated_amplitude_sum += abs(sample);
            total_samples_accumulated++;
        }

        if (audio_packet_counter >= 3) {
            double average_amplitude = 0.0;
            if (total_samples_accumulated > 0) {
                average_amplitude = accumulated_amplitude_sum / total_samples_accumulated;
            }
            int bar_elements_to_fill = (int)((average_amplitude / 8000.0) * VU_METER_WIDTH);
            if (bar_elements_to_fill > VU_METER_WIDTH) bar_elements_to_fill = VU_METER_WIDTH;
            if (bar_elements_to_fill < 0) bar_elements_to_fill = 0;

            EnterCriticalSection(&g_consoleLock);
            SetConsoleTextAttribute(g_hConsoleOut, COLOR_RX);
            printf("\r[");
            for (int b = 0; b < VU_METER_WIDTH; b++) {
                if (b < bar_elements_to_fill) {
                    if (b > (int)(VU_METER_WIDTH * 0.8)) printf("#");
                    else if (b > (int)(VU_METER_WIDTH * 0.5)) printf("=");
                    else printf("-");
                } else {
                    printf(" ");
                }
            }
            printf("] Level: %4.0f | Gain: %3.0f%% %s",
                   average_amplitude, volume_multiplier * 100.0f,
                   should_play ? "      " : "[MUTED-TX]");
            SetConsoleTextAttribute(g_hConsoleOut, g_defaultConsoleAttributes);
            fflush(stdout);
            LeaveCriticalSection(&g_consoleLock);

            audio_packet_counter = 0;
            accumulated_amplitude_sum = 0.0;
            total_samples_accumulated = 0;
        }

        if (should_play) {
            hdr->dwBufferLength = audio_payload_len * sizeof(int16_t);
            hdr->dwFlags &= ~WHDR_DONE;
            waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
            rxCurrentBufferIndex = (rxCurrentBufferIndex + 1) % RX_BUFFER_COUNT;
        }
    }

    return 0;
}

// ==========================================================
// TX SIDE (from HyteraTX-Mic-PTT.c) -- microphone -> RTP push-to-talk
// ==========================================================
#define WAVE_FORMAT_MULAW_TAG 7
#define AUDIO_BUFFER_BYTES    160   // 8000Hz * 0.020s * 1 byte/sample = one 20ms RTP frame
#define NUM_AUDIO_BUFFERS     8

typedef struct {
    SOCKET socket;
    struct sockaddr_in target_addr;
} keepalive_ctx_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t fixed_marker;
    uint16_t seq_num;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t  hytera_pad[16];
    uint8_t  voice_payload[AUDIO_BUFFER_BYTES];
} rtp_packet_t;
#pragma pack(pop)

static uint8_t  rcp_sequence_counter = 0;
static uint16_t rtp_sequence_counter = 0;
static uint32_t rtp_timestamp_counter = 0;

static HWAVEIN g_hWaveIn = NULL;
static WAVEHDR g_waveHeaders[NUM_AUDIO_BUFFERS];
static uint8_t g_audioBuffers[NUM_AUDIO_BUFFERS][AUDIO_BUFFER_BYTES];
static HANDLE  g_hDataEvent = NULL;
static HANDLE  g_hStopEvent = NULL;
static volatile LONG g_stop_requested = 0;

uint8_t get_next_rcp_seq() { return rcp_sequence_counter++; }

void send_call_setup(SOCKET sock, struct sockaddr_in* addr, uint8_t call_type, uint32_t target_id) {
    uint8_t packet[sizeof(CALL_SETUP_TEMPLATE)];
    memcpy(packet, CALL_SETUP_TEMPLATE, sizeof(CALL_SETUP_TEMPLATE));
    packet[5]  = get_next_rcp_seq();
    packet[11] = call_type;
    packet[12] = target_id & 0xFF;
    packet[13] = (target_id >> 8) & 0xFF;
    packet[14] = (target_id >> 16) & 0xFF;
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

void send_ptt_command(SOCKET sock, struct sockaddr_in* addr, int turn_on) {
    uint8_t packet[sizeof(PTT_TEMPLATE)];
    memcpy(packet, PTT_TEMPLATE, sizeof(PTT_TEMPLATE));
    packet[5] = get_next_rcp_seq();
    if (turn_on) { packet[12] = 0x01; packet[13] = 0xEB; }
    else         { packet[12] = 0x00; packet[13] = 0xEC; }
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

// Sends a hand-built SNMPv1 SetRequest to Hytera's private rptRestart OID.
// This replaces the earlier undocumented "stop the watchdog" packet with a
// real, documented mechanism confirmed directly from Hytera's own
// HYTERA-REPEATER-MIB (OID .1.3.6.1.4.1.40297.1.2.2.1.0, community "public",
// value 1 = reset) and verified working via iReasoning MIB Browser.
//
// The packet below is a fixed, hand-encoded BER/ASN.1 byte sequence since
// none of its fields (OID, community, value) ever change:
//   SEQUENCE {
//     version   INTEGER 0            -- SNMPv1
//     community OCTET STRING "public"
//     [3] SetRequest-PDU {           -- context tag 3, i.e. 0xA3
//       request-id    INTEGER 1
//       error-status  INTEGER 0
//       error-index   INTEGER 0
//       variable-bindings SEQUENCE {
//         SEQUENCE {
//           name  OBJECT IDENTIFIER  1.3.6.1.4.1.40297.1.2.2.1.0
//           value INTEGER 1          -- 0 = do nothing, 1 = reset
//         }
//       }
//     }
//   }
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
        printf("[ERROR] Could not create SNMP socket (WSAGetLastError=%d).\n", WSAGetLastError());
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
        printf("[ERROR] Failed to send SNMP restart command (WSAGetLastError=%d).\n", WSAGetLastError());
    } else {
        printf("[SNMP] rptRestart SetRequest sent to %s:161 (community=public, value=1).\n", target_ip);
    }

    closesocket(snmp_sock);
}

// ==========================================================
// SNMP TELEMETRY -- live-polls a set of read-only repeater sensors/alarms
// every few seconds via a proper multi-OID SNMP GetRequest/GetResponse.
// Unlike the single fire-and-forget SetRequest above, this needs generic
// BER encode/decode since we're both building a request with several
// nested SEQUENCEs and parsing whatever length-encoding the repeater's
// GetResponse comes back with.
// ==========================================================

// Writes a BER length field (short or long form) at buf[offset].
// Returns the number of bytes written.
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

// Appends a full TLV (tag + length + content) to dest starting at dest_offset.
// Returns the new offset (dest_offset + bytes written).
static int ber_append_tlv(uint8_t* dest, int dest_offset, uint8_t tag, const uint8_t* content, int content_len) {
    dest[dest_offset] = tag;
    int len_bytes = ber_write_length(dest, dest_offset + 1, content_len);
    int header_len = 1 + len_bytes;
    if (content_len > 0) memcpy(dest + dest_offset + header_len, content, content_len);
    return dest_offset + header_len + content_len;
}

// Reads one BER TLV starting at buf[*offset]. On success, fills out_tag/
// out_content/out_content_len and advances *offset past the whole TLV.
// Returns 0 on failure (truncated/malformed) without advancing further.
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

// Standard BER variable-length two's-complement INTEGER decode.
static long ber_decode_integer(const uint8_t* content, int len) {
    long value = (len > 0 && (content[0] & 0x80)) ? -1 : 0;
    for (int i = 0; i < len; i++) value = (value << 8) | content[i];
    return value;
}

// Hytera's OCTET STRING(4) "should be float format" values are a raw
// 32-bit IEEE-754 float in little-endian byte order -- confirmed by
// decoding a real captured rptPaTemprature reading (0x00 00 D8 41 ->
// reversed 41 D8 00 00 -> 27.0, a plausible PA temperature). Windows on
// x86/x64 stores floats the same way in memory, so no byte-swap needed.
static float decode_hytera_float(const uint8_t* content, int len) {
    float f = 0.0f;
    if (len == 4) memcpy(&f, content, 4);
    return f;
}

typedef enum { TELEM_FLOAT, TELEM_INT, TELEM_ALARM_TRISTATE, TELEM_ALARM_BISTATE } telem_type_t;

typedef struct {
    const char* label;
    uint8_t branch;   // 1 = rptAlarmInfo, 2 = rptDataInfo
    uint8_t item;     // item index within that branch
    telem_type_t type;
    const char* unit;
    int unsupported;  // Hytera's own MIB marks this object "not supported yet"
} telem_oid_t;

// OID = .1.3.6.1.4.1.40297.1.2.1.<branch>.<item>.0 for every entry here --
// all confirmed against HYTERA-REPEATER-MIB.
static const telem_oid_t TELEM_OIDS[] = {
    { "Voltage",         2, 1,  TELEM_FLOAT,          "V",  0 },
    { "Voltage Alarm",   1, 1,  TELEM_ALARM_TRISTATE,  "",  0 },
    { "PA Temp",         2, 2,  TELEM_FLOAT,          "C",  0 },
    { "Temp Alarm",      1, 2,  TELEM_ALARM_TRISTATE,  "",  0 },
    { "Fan Speed",       2, 3,  TELEM_INT,              "",  1 },
    { "Fan Alarm",       1, 3,  TELEM_ALARM_BISTATE,   "",  1 },
    { "VSWR",            2, 4,  TELEM_FLOAT,           "",  0 },
    { "VSWR Alarm",      1, 6,  TELEM_ALARM_BISTATE,   "",  0 },
    { "Battery Voltage", 2, 13, TELEM_FLOAT,           "V", 1 },
    { "Battery Alarm",   1, 9,  TELEM_ALARM_BISTATE,   "",  0 },
    { "Slot1 RSSI",      2, 9,  TELEM_INT,             "dB", 0 },
    { "Slot2 RSSI",      2, 10, TELEM_INT,             "dB", 0 },
};
#define TELEM_COUNT (sizeof(TELEM_OIDS) / sizeof(TELEM_OIDS[0]))

// Builds the fixed 14-byte OID content .1.3.6.1.4.1.40297.1.2.1.<branch>.<item>.0
// (excludes the OID tag/length -- caller wraps it with ber_append_tlv).
static void build_telem_oid_content(uint8_t* out, uint8_t branch, uint8_t item) {
    static const uint8_t PREFIX[] = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02, 0x01 };
    memcpy(out, PREFIX, sizeof(PREFIX));
    out[sizeof(PREFIX) + 0] = branch;
    out[sizeof(PREFIX) + 1] = item;
    out[sizeof(PREFIX) + 2] = 0x00; // scalar instance
}

// Builds a full SNMPv1 GetRequest for all of TELEM_OIDS in one packet.
// Returns the total packet length written into out_buf.
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

    uint8_t reqid[3]  = { 0x02, 0x01, (uint8_t)(request_id & 0xFF) };
    uint8_t errstat[3] = { 0x02, 0x01, 0x00 };
    uint8_t errindex[3] = { 0x02, 0x01, 0x00 };

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

// Parses a GetResponse and extracts each varbind's value in the same
// order the request was sent, matching TELEM_OIDS by index -- SNMP
// agents are required to echo varbinds back in request order.
// Returns 1 on success, 0 if the packet couldn't be parsed at all.
static int parse_telem_getresponse(const uint8_t* buf, int buf_len, float* out_floats, long* out_ints, int* out_is_float) {
    int off = 0;
    uint8_t tag; const uint8_t* content; int clen;

    // Outer SEQUENCE (whole message)
    if (!ber_read_tlv(buf, buf_len, &off, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* msg = content; int msg_len = clen; int mo = 0;

    // version, community -- skip over both
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // version
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0; // community

    // PDU (should be GetResponse-PDU, tag 0xA2)
    if (!ber_read_tlv(msg, msg_len, &mo, &tag, &content, &clen)) return 0;
    if (tag != 0xA2) return 0; // not a GetResponse -- likely an error PDU or malformed reply
    const uint8_t* pdu = content; int pdu_len = clen; int po = 0;

    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0; // request-id
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0; // error-status
    long error_status = ber_decode_integer(content, clen);
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen)) return 0; // error-index
    if (error_status != 0) return 0; // agent reported an error -- OIDs must be wrong

    // variable-bindings SEQUENCE
    if (!ber_read_tlv(pdu, pdu_len, &po, &tag, &content, &clen) || tag != 0x30) return 0;
    const uint8_t* vbl = content; int vbl_len = clen; int vo = 0;

    for (size_t i = 0; i < TELEM_COUNT; i++) {
        if (!ber_read_tlv(vbl, vbl_len, &vo, &tag, &content, &clen) || tag != 0x30) return 0; // one VarBind
        const uint8_t* vb = content; int vb_len = clen; int vbo = 0;

        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // OID -- trust ordering, don't need it
        if (!ber_read_tlv(vb, vb_len, &vbo, &tag, &content, &clen)) return 0; // value

        if (tag == 0x04) { // OCTET STRING -- Hytera's float encoding
            out_floats[i] = decode_hytera_float(content, clen);
            out_is_float[i] = 1;
        } else if (tag == 0x02) { // INTEGER
            out_ints[i] = ber_decode_integer(content, clen);
            out_is_float[i] = 0;
        } else {
            return 0; // unexpected type for this OID
        }
    }
    return 1;
}

// Renders one alarm/state value as a short word, colored yellow if it's
// signalling a problem.
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

// Polls all telemetry OIDs every few seconds and prints a compact status
// block, pairing each value with its alarm the same way the OIDs were
// originally grouped. Runs until g_app_running is cleared (same shutdown
// flag the RX thread uses).
DWORD WINAPI SnmpMonitorThread(LPVOID lpParam) {
    const char* target_ip = (const char*)lpParam;

    SOCKET snmp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmp_sock == INVALID_SOCKET) {
        safe_printf("[SNMP] Could not create telemetry socket -- monitoring disabled.\n");
        return 0;
    }
    DWORD snmp_timeout = 2000;
    setsockopt(snmp_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&snmp_timeout, sizeof(snmp_timeout));

    struct sockaddr_in snmp_addr;
    memset(&snmp_addr, 0, sizeof(snmp_addr));
    snmp_addr.sin_family = AF_INET;
    snmp_addr.sin_port = htons(161);
    inet_pton(AF_INET, target_ip, &snmp_addr.sin_addr);

    int request_id = 100;

    while (InterlockedCompareExchange(&g_app_running, 0, 0)) {
        uint8_t request[1100];
        int req_len = build_telem_getrequest(request, request_id++);

        sendto(snmp_sock, (const char*)request, req_len, 0, (struct sockaddr*)&snmp_addr, sizeof(snmp_addr));

        uint8_t response[1500];
        struct sockaddr_in from_addr; int from_len = sizeof(from_addr);
        int n = recvfrom(snmp_sock, (char*)response, sizeof(response), 0, (struct sockaddr*)&from_addr, &from_len);

        if (n > 0) {
            float floats[TELEM_COUNT]; long ints[TELEM_COUNT]; int is_float[TELEM_COUNT];
            if (parse_telem_getresponse(response, n, floats, ints, is_float)) {
                EnterCriticalSection(&g_consoleLock);
                printf("\n[SNMP] --- Repeater Telemetry ---\n");

                printf("  Voltage:  %5.2f %-2s ", floats[0], TELEM_OIDS[0].unit);
                print_telem_alarm_word(ints[1], TELEM_ALARM_TRISTATE);
                printf("   PA Temp: %5.1f %-2s ", floats[2], TELEM_OIDS[2].unit);
                print_telem_alarm_word(ints[3], TELEM_ALARM_TRISTATE);
                printf("\n");

                if (TELEM_OIDS[4].unsupported) {
                    printf("  Fan Speed: (unsupported by this repeater)");
                } else {
                    printf("  Fan Speed: %ld ", ints[4]);
                    print_telem_alarm_word(ints[5], TELEM_ALARM_BISTATE);
                }
                printf("   VSWR: %5.2f ", floats[6]);
                print_telem_alarm_word(ints[7], TELEM_ALARM_BISTATE);
                printf("\n");

                if (TELEM_OIDS[8].unsupported) {
                    printf("  Battery Voltage: (unsupported by this repeater) ");
                } else {
                    printf("  Battery Voltage: %5.2f %-2s ", floats[8], TELEM_OIDS[8].unit);
                }
                print_telem_alarm_word(ints[9], TELEM_ALARM_BISTATE);
                printf("\n");

                printf("  Slot1 RSSI: %ld dB   Slot2 RSSI: %ld dB\n", ints[10], ints[11]);
                printf("[SNMP] ---------------------------\n");
                fflush(stdout);
                LeaveCriticalSection(&g_consoleLock);
            } else {
                safe_printf("[SNMP] Telemetry response could not be parsed (unexpected format or error status).\n");
            }
        } else {
            safe_printf("[SNMP] No telemetry response from repeater (timeout).\n");
        }

        // Sleep in short increments so Esc/shutdown is responsive rather
        // than waiting out a full 5-second sleep.
        for (int waited = 0; waited < 5000 && InterlockedCompareExchange(&g_app_running, 0, 0); waited += 100) {
            Sleep(100);
        }
    }

    closesocket(snmp_sock);
    return 0;
}

VOID CALLBACK SendKeepaliveCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer) {
    keepalive_ctx_t *ctx = (keepalive_ctx_t*)Context;
    sendto(ctx->socket, (const char*)KEEP_ALIVE_PAYLOAD, sizeof(KEEP_ALIVE_PAYLOAD), 0,
           (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

int start_mic_capture() {
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_MULAW_TAG;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = 8000;
    wfx.nAvgBytesPerSec = 8000;
    wfx.nBlockAlign     = 1;
    wfx.wBitsPerSample  = 8;
    wfx.cbSize          = 0;

    g_hDataEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hDataEvent || !g_hStopEvent) {
        printf("[ERROR] Failed to create synchronization events.\n");
        return 0;
    }

    MMRESULT res = waveInOpen(&g_hWaveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)g_hDataEvent, 0, CALLBACK_EVENT);
    if (res != MMSYSERR_NOERROR) {
        char errText[256];
        waveInGetErrorTextA(res, errText, sizeof(errText));
        printf("[ERROR] waveInOpen failed: %s\n", errText);
        printf("[HINT] Your default recording device may not support direct G.711 u-law capture.\n");
        return 0;
    }

    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        memset(&g_waveHeaders[i], 0, sizeof(WAVEHDR));
        g_waveHeaders[i].lpData         = (LPSTR)g_audioBuffers[i];
        g_waveHeaders[i].dwBufferLength = AUDIO_BUFFER_BYTES;
        waveInPrepareHeader(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
    }

    res = waveInStart(g_hWaveIn);
    if (res != MMSYSERR_NOERROR) {
        printf("[ERROR] waveInStart failed (code %d).\n", res);
        return 0;
    }

    printf("[MIC] Microphone capture started (G.711 u-law, 8000Hz, mono).\n");
    return 1;
}

void stop_mic_capture() {
    if (!g_hWaveIn) return;
    waveInStop(g_hWaveIn);
    waveInReset(g_hWaveIn);
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        waveInUnprepareHeader(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
    }
    waveInClose(g_hWaveIn);
    g_hWaveIn = NULL;
}

typedef struct {
    SOCKET rtp_sock;
    struct sockaddr_in* remote_rtp_addr;
} sender_thread_ctx_t;

DWORD WINAPI AudioSenderThread(LPVOID lpParam) {
    sender_thread_ctx_t* ctx = (sender_thread_ctx_t*)lpParam;
    HANDLE waitHandles[2] = { g_hDataEvent, g_hStopEvent };

    int tx_packet_counter = 0;
    double tx_accumulated_amplitude_sum = 0.0;
    long long tx_total_samples_accumulated = 0;

    while (1) {
        WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
            if (!(g_waveHeaders[i].dwFlags & WHDR_DONE)) continue;

            if (InterlockedCompareExchange(&g_transmitting, 0, 0)) {
                rtp_packet_t audio_pkt;
                memset(&audio_pkt, 0, sizeof(audio_pkt));

                audio_pkt.fixed_marker = htons(0x9000);
                audio_pkt.seq_num      = htons(rtp_sequence_counter++);
                audio_pkt.timestamp    = htonl(rtp_timestamp_counter);
                audio_pkt.ssrc         = 0;
                audio_pkt.hytera_pad[1] = 0x15;
                audio_pkt.hytera_pad[3] = 0x03;

                DWORD bytesRecorded = g_waveHeaders[i].dwBytesRecorded;
                if (bytesRecorded > AUDIO_BUFFER_BYTES) bytesRecorded = AUDIO_BUFFER_BYTES;

                // Apply TX gain in the linear PCM domain (u-law is logarithmic,
                // so scaling the raw byte directly would distort rather than
                // amplify/attenuate it) -- decode, scale+clamp, re-encode.
                float tx_gain = g_tx_volume_multiplier;
                uint8_t *mic_bytes = (uint8_t*)g_waveHeaders[i].lpData;
                for (DWORD s = 0; s < bytesRecorded; s++) {
                    int16_t linear = ulaw_to_pcm_lut[mic_bytes[s]];
                    float amplified = (float)linear * tx_gain;
                    int16_t clamped;
                    if (amplified > 32767.0f) clamped = 32767;
                    else if (amplified < -32768.0f) clamped = -32768;
                    else clamped = (int16_t)amplified;

                    audio_pkt.voice_payload[s] = linear_to_ulaw(clamped);

                    tx_accumulated_amplitude_sum += abs(clamped);
                    tx_total_samples_accumulated++;
                }
                if (bytesRecorded < AUDIO_BUFFER_BYTES) {
                    memset(audio_pkt.voice_payload + bytesRecorded, 0xFF, AUDIO_BUFFER_BYTES - bytesRecorded);
                }

                sendto(ctx->rtp_sock, (const char*)&audio_pkt, sizeof(audio_pkt), 0,
                       (struct sockaddr*)ctx->remote_rtp_addr, sizeof(*ctx->remote_rtp_addr));

                rtp_timestamp_counter += AUDIO_BUFFER_BYTES;

                // Render the TX VU meter -- same style/cadence as RX, just red.
                tx_packet_counter++;
                if (tx_packet_counter >= 3) {
                    double average_amplitude = 0.0;
                    if (tx_total_samples_accumulated > 0) {
                        average_amplitude = tx_accumulated_amplitude_sum / tx_total_samples_accumulated;
                    }
                    int bar_elements_to_fill = (int)((average_amplitude / 8000.0) * VU_METER_WIDTH);
                    if (bar_elements_to_fill > VU_METER_WIDTH) bar_elements_to_fill = VU_METER_WIDTH;
                    if (bar_elements_to_fill < 0) bar_elements_to_fill = 0;

                    EnterCriticalSection(&g_consoleLock);
                    SetConsoleTextAttribute(g_hConsoleOut, COLOR_TX);
                    printf("\r[");
                    for (int b = 0; b < VU_METER_WIDTH; b++) {
                        if (b < bar_elements_to_fill) {
                            if (b > (int)(VU_METER_WIDTH * 0.8)) printf("#");
                            else if (b > (int)(VU_METER_WIDTH * 0.5)) printf("=");
                            else printf("-");
                        } else {
                            printf(" ");
                        }
                    }
                    printf("] TX Level: %4.0f | TX Gain: %3.0f%%      ", average_amplitude, tx_gain * 100.0f);
                    SetConsoleTextAttribute(g_hConsoleOut, g_defaultConsoleAttributes);
                    fflush(stdout);
                    LeaveCriticalSection(&g_consoleLock);

                    tx_packet_counter = 0;
                    tx_accumulated_amplitude_sum = 0.0;
                    tx_total_samples_accumulated = 0;
                }
            }

            g_waveHeaders[i].dwFlags &= ~WHDR_DONE;
            g_waveHeaders[i].dwBytesRecorded = 0;
            waveInAddBuffer(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
        }

        if (InterlockedCompareExchange(&g_stop_requested, 0, 0)) break;
    }
    return 0;
}

// ==========================================================
// MAIN -- brings RX and TX up together on the shared RTP socket
// ==========================================================
int main(int argc, char *argv[]) {
    InitializeCriticalSection(&g_consoleLock);
    timeBeginPeriod(1);
    init_ulaw_lut();

    // Hide the blinking text cursor -- with the VU meter constantly redrawing
    // via carriage returns, the blinking cursor tends to leave visible
    // "ghost" artifacts in the console. Purely cosmetic, no functional effect.
    {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_CURSOR_INFO cursorInfo;
        if (GetConsoleCursorInfo(hConsole, &cursorInfo)) {
            cursorInfo.bVisible = FALSE;
            SetConsoleCursorInfo(hConsole, &cursorInfo);
        }
    }

    // Capture the console's own default colour (standard MS-DOS light grey on
    // black) so the RX/TX VU meters can reset back to it after each coloured
    // line, rather than assuming what "default" means.
    g_hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(g_hConsoleOut, &csbi)) {
            g_defaultConsoleAttributes = csbi.wAttributes;
        } else {
            g_defaultConsoleAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // fallback: plain white
        }
    }
    
	// === INSERT YOUR FIRST LINE HERE ===
    printf("[AUTHOR] Hytera Transceiver by Rob Thompson 2E0RPT.\n"); 
    fflush(stdout);
	
    if (!init_playback_device(8000)) {
        timeEndPeriod(1);
        return -1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock initialization failed.\n");
        shutdown_playback_device();
        timeEndPeriod(1);
        return -1;
    }

    SOCKET rcp_sock, rtp_sock;
    struct sockaddr_in local_rcp_addr, local_rtp_addr;
    struct sockaddr_in remote_rcp_addr, remote_rtp_addr;

    if ((rcp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET ||
        (rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        WSACleanup(); shutdown_playback_device(); timeEndPeriod(1);
        return -1;
    }

    // RTP socket needs a short receive timeout so the RX thread can
    // periodically check g_app_running and shut down cleanly on Esc.
    DWORD rx_timeout = 100;
    setsockopt(rtp_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rx_timeout, sizeof(rx_timeout));

    memset(&local_rcp_addr, 0, sizeof(local_rcp_addr));
    local_rcp_addr.sin_family = AF_INET;
    local_rcp_addr.sin_port = htons(PORT_RCP);
    inet_pton(AF_INET, LOCAL_IP, &local_rcp_addr.sin_addr);

    // Bind RTP socket to ANY local interface (matches your proven-working RX code) --
    // sendto() still explicitly targets REPEATER_IP regardless of bind address.
    memset(&local_rtp_addr, 0, sizeof(local_rtp_addr));
    local_rtp_addr.sin_family = AF_INET;
    local_rtp_addr.sin_port = htons(PORT_RTP);
    local_rtp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(rcp_sock, (struct sockaddr*)&local_rcp_addr, sizeof(local_rcp_addr)) == SOCKET_ERROR ||
        bind(rtp_sock, (struct sockaddr*)&local_rtp_addr, sizeof(local_rtp_addr)) == SOCKET_ERROR) {
        printf("[ERROR] Socket port bindings failed.\n");
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup(); shutdown_playback_device(); timeEndPeriod(1);
        return -1;
    }

    // ------------------------------------------------------------
    // Resolve the repeater's IP address: either take it from argv[1],
    // or -- if no argument was given -- wait here for the repeater to
    // send us something first and learn its address from that packet's
    // source, the same way the original standalone RX program worked.
    // ------------------------------------------------------------
    char repeater_ip_str[INET_ADDRSTRLEN];

    if (argc >= 2) {
        struct in_addr testAddr;
        if (inet_pton(AF_INET, argv[1], &testAddr) != 1) {
            printf("[ERROR] '%s' is not a valid IPv4 address.\n", argv[1]);
            printf("        Usage: %s [repeater_ip]\n", argv[0]);
            closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup(); shutdown_playback_device(); timeEndPeriod(1);
            return -1;
        }
        strncpy(repeater_ip_str, argv[1], sizeof(repeater_ip_str) - 1);
        repeater_ip_str[sizeof(repeater_ip_str) - 1] = '\0';
        printf("[SYSTEM] Using repeater IP from command line: %s\n", repeater_ip_str);
    } else {
        printf("[SYSTEM] No repeater IP given -- waiting for the repeater to contact us on port %d...\n", PORT_RTP);
        printf("[SYSTEM] (Pass it as an argument instead to skip this wait, e.g. %s 192.168.1.167)\n", argv[0]);

        struct sockaddr_in discovery_addr;
        int discovery_addr_len = sizeof(discovery_addr);
        char discovery_buf[NETWORK_BUF_SZ];
        int waited_ms = 0;

        while (1) {
            discovery_addr_len = sizeof(discovery_addr);
            int n = recvfrom(rtp_sock, discovery_buf, sizeof(discovery_buf), 0,
                              (struct sockaddr*)&discovery_addr, &discovery_addr_len);
            if (n > 0) {
                inet_ntop(AF_INET, &discovery_addr.sin_addr, repeater_ip_str, sizeof(repeater_ip_str));
                printf("[SYSTEM] Discovered repeater IP: %s\n", repeater_ip_str);
                break;
            }
            // recvfrom timed out (SO_RCVTIMEO=100ms was set above) -- keep waiting
            waited_ms += 100;
            if (waited_ms % 5000 == 0) {
                printf("[SYSTEM] Still waiting for the repeater to speak first (%d s)...\n", waited_ms / 1000);
            }
        }
        // Note: this first packet is consumed here for discovery and not
        // passed on to the RX thread -- negligible, just one lost frame.
    }

    memset(&remote_rcp_addr, 0, sizeof(remote_rcp_addr));
    remote_rcp_addr.sin_family = AF_INET;
    remote_rcp_addr.sin_port = htons(PORT_RCP);
    inet_pton(AF_INET, repeater_ip_str, &remote_rcp_addr.sin_addr);

    memset(&remote_rtp_addr, 0, sizeof(remote_rtp_addr));
    remote_rtp_addr.sin_family = AF_INET;
    remote_rtp_addr.sin_port = htons(PORT_RTP);
    inet_pton(AF_INET, repeater_ip_str, &remote_rtp_addr.sin_addr);

    printf("[SYSTEM] Launching hardware link wake sequence...\n");
    sendto(rcp_sock, (const char*)WAKE_CALL_PAYLOAD, sizeof(WAKE_CALL_PAYLOAD), 0, (struct sockaddr*)&remote_rcp_addr, sizeof(remote_rcp_addr));
    sendto(rtp_sock, (const char*)WAKE_CALL_PAYLOAD, sizeof(WAKE_CALL_PAYLOAD), 0, (struct sockaddr*)&remote_rtp_addr, sizeof(remote_rtp_addr));

    keepalive_ctx_t rcp_ctx = { rcp_sock, remote_rcp_addr };
    keepalive_ctx_t rtp_ctx = { rtp_sock, remote_rtp_addr };
    PTP_TIMER rcp_timer = CreateThreadpoolTimer(SendKeepaliveCallback, &rcp_ctx, NULL);
    PTP_TIMER rtp_timer = CreateThreadpoolTimer(SendKeepaliveCallback, &rtp_ctx, NULL);

    if (rcp_timer != NULL && rtp_timer != NULL) {
        FILETIME ftDueTime;
        ULARGE_INTEGER ulDueTime;
        ulDueTime.QuadPart = 0;
        ftDueTime.dwHighDateTime = ulDueTime.HighPart;
        ftDueTime.dwLowDateTime = ulDueTime.LowPart;
        SetThreadpoolTimer(rcp_timer, &ftDueTime, 5000, 0);
        SetThreadpoolTimer(rtp_timer, &ftDueTime, 5000, 0);
        printf("[SYSTEM] Asynchronous 5-second keepalives armed (covers RX heartbeat too).\n");
    }

    printf("[SYSTEM] Holding link stabilization window for 4 seconds...\n");
    Sleep(4000);

    // Start RX thread -- begins listening/decoding/playing immediately
    HANDLE hRxThread = CreateThread(NULL, 0, RXThreadFunc, &rtp_sock, 0, NULL);
    printf("[RX] Listening for Hytera repeater UDP stream on port %d...\n", PORT_RTP);
    printf("[RX] Use   UP/DOWN  keys to adjust RX gain.\n");
	printf("[TX] Use LEFT/RIGHT keys to adjust TX gain.\n");

    // Start SNMP telemetry monitor thread -- polls repeater sensors/alarms
    // every ~5 seconds and prints a status block until shutdown.
    HANDLE hSnmpThread = CreateThread(NULL, 0, SnmpMonitorThread, (LPVOID)repeater_ip_str, 0, NULL);
    printf("[SNMP] Telemetry monitoring started (Voltage/Temp/VSWR/Battery/RSSI).\n");

    // Initial Call Setup (also re-sent before every subsequent key-up below,
    // since the repeater tears the call down as soon as PTT de-keys)
    printf("[RCP] Sending Call Setup Envelope for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, &remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);

    if (!start_mic_capture()) {
        printf("[ERROR] Could not start microphone capture.\n");
        InterlockedExchange(&g_app_running, 0);
        WaitForSingleObject(hRxThread, 2000);
        if (hSnmpThread) { WaitForSingleObject(hSnmpThread, 2000); CloseHandle(hSnmpThread); }
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup(); shutdown_playback_device(); timeEndPeriod(1);
        return -1;
    }

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    sender_thread_ctx_t sender_ctx = { rtp_sock, &remote_rtp_addr };
    HANDLE hSenderThread = CreateThread(NULL, 0, AudioSenderThread, &sender_ctx, 0, NULL);

    // Safety check: if Caps Lock is already ON when we get here (left on from
    // a previous session, or just habit), don't let that immediately trigger
    // a transmission with no warning. Block here until it's turned off.
    if (GetKeyState(VK_CAPITAL) & 0x0001) {
        printf("[WARNING] Caps Lock is already ON. Please turn it OFF before transmitting.\n");
        printf("[WARNING] Transmission is suspended until Caps Lock is OFF.\n");
        int warned_recently = 0;
        while (GetKeyState(VK_CAPITAL) & 0x0001) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                // Allow quitting straight away even while stuck in this wait
                printf("[SYSTEM] Exiting before transmission was ever enabled.\n");
                InterlockedExchange(&g_stop_requested, 1);
                SetEvent(g_hStopEvent);
                if (hSenderThread) { WaitForSingleObject(hSenderThread, 2000); CloseHandle(hSenderThread); }
                stop_mic_capture();
                InterlockedExchange(&g_app_running, 0);
                if (hRxThread) { WaitForSingleObject(hRxThread, 2000); CloseHandle(hRxThread); }
                if (hSnmpThread) { WaitForSingleObject(hSnmpThread, 2000); CloseHandle(hSnmpThread); }
                if (rcp_timer) { SetThreadpoolTimer(rcp_timer, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(rcp_timer, TRUE); CloseThreadpoolTimer(rcp_timer); }
                if (rtp_timer) { SetThreadpoolTimer(rtp_timer, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(rtp_timer, TRUE); CloseThreadpoolTimer(rtp_timer); }
                if (g_hDataEvent) CloseHandle(g_hDataEvent);
                if (g_hStopEvent) CloseHandle(g_hStopEvent);
                closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup();
                shutdown_playback_device(); timeEndPeriod(1); DeleteCriticalSection(&g_consoleLock);
                return 0;
            }
            if (!warned_recently) {
                printf("[WARNING] Still waiting for Caps Lock to be turned OFF...\n");
            }
            Sleep(100);
            warned_recently = (warned_recently + 1) % 30; // gentle reminder roughly every 3s
        }
        printf("[SYSTEM] Caps Lock is now OFF. Transmission enabled.\n");
    }

    printf("[PTT] Ready. Turn CAPS LOCK ON to transmit, OFF to stop. Press ESC to quit.\n");
    //printf("[PTT] Use LEFT/RIGHT keys to adjust TX (microphone) volume.\n");

    int wasKeyed = 0;
    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            // Force transmission off cleanly if they press ESC while transmitting
            if (wasKeyed) {
                InterlockedExchange(&g_transmitting, 0);
                safe_printf("\r%95s\r[RCP] PTT De-Key (cancelling for exit)...", "");
                send_ptt_command(rcp_sock, &remote_rcp_addr, 0);
                wasKeyed = 0;
            }

            int should_reboot = 0;
            safe_printf("\n[SYSTEM] Do you wish to reboot the repeater before you exit [y/N]?\n");

            // Visible 5-second timeout tracking loop
            for (int seconds_left = 5; seconds_left > 0; seconds_left--) {
                safe_printf("\rDefaulting to 'No' in %d seconds... (Press Y or N) ", seconds_left);
                
                // Poll keys rapidly over a 1-second interval
                for (int poll = 0; poll < 100; poll++) {
                    if ((GetAsyncKeyState('Y') & 0x8000) || (GetAsyncKeyState('y') & 0x8000)) {
                        should_reboot = 1;
                        break;
                    }
                    if ((GetAsyncKeyState('N') & 0x8000) || (GetAsyncKeyState('n') & 0x8000)) {
                        should_reboot = 0;
                        poll = 100; // Break inner poll
                    }
                    Sleep(10);
                }
                if (should_reboot) break; // User hit 'Y', exit countdown immediately
            }

            if (should_reboot) {
                safe_printf("\r\n[SNMP] Sending rptRestart command to repeater...\n");
                send_snmp_repeater_restart(repeater_ip_str);
                Sleep(200); // Give Winsock a brief moment to push the packet out
            } else {
                safe_printf("\r\n[SYSTEM] Exiting cleanly without rebooting.\n");
            }
            break; // Break the main while(1) loop to run standard shutdown logic
        }

        // --- TX volume hotkeys (same edge-triggered pattern as RX's Up/Down) ---
        if (GetForegroundWindow() == GetConsoleWindow()) {
            if (GetAsyncKeyState(VK_RIGHT) & 0x0001) {
                float v = g_tx_volume_multiplier + 0.5f;
                if (v > 5.0f) v = 5.0f;
                g_tx_volume_multiplier = v;
                safe_printf("\r[TX Volume Set]: %3.0f%%                                                      \n", v * 100.0f);
            }
            if (GetAsyncKeyState(VK_LEFT) & 0x0001) {
                float v = g_tx_volume_multiplier - 0.5f;
                if (v < 0.0f) v = 0.0f;
                g_tx_volume_multiplier = v;
                safe_printf("\r[TX Volume Set]: %3.0f%%                                                      \n", v * 100.0f);
            }
        }

        int spaceDown = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

        if (spaceDown && !wasKeyed) {
            safe_printf("[RCP] Re-establishing Call Setup for Talkgroup %d...\n", TARGET_TALKGROUP);
            send_call_setup(rcp_sock, &remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
            Sleep(100);
            safe_printf("[RCP] PTT Key-Up (transmitting)...\n");
            send_ptt_command(rcp_sock, &remote_rcp_addr, 1);
            InterlockedExchange(&g_transmitting, 1);
            wasKeyed = 1;
        } else if (!spaceDown && wasKeyed) {
            InterlockedExchange(&g_transmitting, 0);
            
            // 1. Wipe the remaining TX VU meter using a carriage return and blank spaces
            safe_printf("\r%95s\r", ""); 
            
            // 2. Print the de-key status message cleanly on its own line
            safe_printf("[RCP] PTT De-Key (idle)...\n");
            
            send_ptt_command(rcp_sock, &remote_rcp_addr, 0);
            wasKeyed = 0;
        }


        Sleep(10);
    }

    if (wasKeyed) {
        InterlockedExchange(&g_transmitting, 0);
        printf("[RCP] PTT De-Key (session ending)...\n");
        send_ptt_command(rcp_sock, &remote_rcp_addr, 0);
    }

    // Shut down TX sender thread
    InterlockedExchange(&g_stop_requested, 1);
    SetEvent(g_hStopEvent);
    if (hSenderThread) { WaitForSingleObject(hSenderThread, 2000); CloseHandle(hSenderThread); }
    stop_mic_capture();

    // Shut down RX thread
    InterlockedExchange(&g_app_running, 0);
    if (hRxThread) { WaitForSingleObject(hRxThread, 2000); CloseHandle(hRxThread); }
    if (hSnmpThread) { WaitForSingleObject(hSnmpThread, 2000); CloseHandle(hSnmpThread); }

    printf("[SYSTEM] Streams stopped.\n");

    if (rcp_timer) {
        SetThreadpoolTimer(rcp_timer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(rcp_timer, TRUE);
        CloseThreadpoolTimer(rcp_timer);
    }
    if (rtp_timer) {
        SetThreadpoolTimer(rtp_timer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(rtp_timer, TRUE);
        CloseThreadpoolTimer(rtp_timer);
    }

    if (g_hDataEvent) CloseHandle(g_hDataEvent);
    if (g_hStopEvent) CloseHandle(g_hStopEvent);

    // Restore the blinking cursor before handing the console back
    {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_CURSOR_INFO cursorInfo;
        if (GetConsoleCursorInfo(hConsole, &cursorInfo)) {
            cursorInfo.bVisible = TRUE;
            SetConsoleCursorInfo(hConsole, &cursorInfo);
        }
    }

    closesocket(rcp_sock);
    closesocket(rtp_sock);
    WSACleanup();
    shutdown_playback_device();
    timeEndPeriod(1);
    DeleteCriticalSection(&g_consoleLock);

    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

    printf("[DONE] Transceiver session completed cleanly.\n");
    return 0;
}
