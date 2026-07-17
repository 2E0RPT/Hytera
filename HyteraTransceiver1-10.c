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
// remove this blank
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
// remove rthis blank
void send_ptt_command(SOCKET sock, struct sockaddr_in* addr, int turn_on) {
    uint8_t packet[sizeof(PTT_TEMPLATE)];
    memcpy(packet, PTT_TEMPLATE, sizeof(PTT_TEMPLATE));
    packet[5] = get_next_rcp_seq();
    if (turn_on) { packet[12] = 0x01; packet[13] = 0xEB; }
    else         { packet[12] = 0x00; packet[13] = 0xEC; }
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
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

// Make sure we turn Caps Lock off.
// Turn Caps Lock Off Function Definition
void turn_off_caps_lock(void) {
    // Check if Caps Lock is currently turned ON
    if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0) {
        INPUT inputs[2];
        ZeroMemory(inputs, sizeof(inputs));

        // 1. Configure the simulated Key Down event
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_CAPITAL;
        inputs[0].ki.dwFlags = 0; // 0 designates Key Down
    
        // 2. Configure the simulated Key Up event
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_CAPITAL;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        // Broadcast both events to the Windows OS input stream
        SendInput(2, inputs, sizeof(INPUT));
		// Grab the console handle and switch text to Red (Intensity + Red)
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, FOREGROUND_INTENSITY | FOREGROUND_RED);
        
        // Let user know Caps Lock was turned off.
        printf("[CAPS L] Caps Lock has been turned off because that is your PTT key\n");
    
        // Reset console color back to standard White (Red + Green + Blue)
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    }
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

    // Publisher information
	printf("[AUTHOR] HyteraTransceiver made by Rob Thompson 2E0RPT...\n");
	
	// Lets turn Caps Lock off so we dont jump into TX.
    turn_off_caps_lock();
	// Phew, That was a close one.
	
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

    // Initial Call Setup (also re-sent before every subsequent key-up below,
    // since the repeater tears the call down as soon as PTT de-keys)
    printf("[RCP] Sending Call Setup Envelope for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, &remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);

    if (!start_mic_capture()) {
        printf("[ERROR] Could not start microphone capture.\n");
        InterlockedExchange(&g_app_running, 0);
        WaitForSingleObject(hRxThread, 2000);
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup(); shutdown_playback_device(); timeEndPeriod(1);
        return -1;
    }

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    sender_thread_ctx_t sender_ctx = { rtp_sock, &remote_rtp_addr };
    HANDLE hSenderThread = CreateThread(NULL, 0, AudioSenderThread, &sender_ctx, 0, NULL);

    printf("[PTT] Ready. Hold SPACE to transmit, release to stop. Press ESC to quit.\n");
    printf("[PTT] Use LEFT/RIGHT keys to adjust TX (microphone) volume.\n");

    int wasKeyed = 0;
    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

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

        int spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

        // Check if Caps Lock is toggled on (using Windows API)
        int capsLockOn = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

        if (capsLockOn && !wasKeyed) {
            safe_printf("[RCP] Re-establishing Call Setup for Talkgroup %d...\n", TARGET_TALKGROUP);
            send_call_setup(rcp_sock, &remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
            Sleep(100);
            safe_printf("[RCP] PTT Key-Up (transmitting)...\n");
            send_ptt_command(rcp_sock, &remote_rcp_addr, 1);
            InterlockedExchange(&g_transmitting, 1);
            wasKeyed = 1;
        } else if (!capsLockOn && wasKeyed) {
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

    printf("[DONE] Transceiver session completed cleanly.\n");
    return 0;
}
