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
#include <windows.h> // Also brings in the serial-port APIs used below (DCB,
                     // COMMTIMEOUTS, GetCommModemStatus, EscapeCommFunction) --
                     // no extra include or extra -l link flag needed for those.
#include <mmsystem.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ==========================================================
// HyteraConnect -- lets external "keying software" on a SECOND computer
// control this repeater link over a plain serial null-modem cable, with
// audio carried separately over two analog cables (soundcard line-out on
// one PC -> line-in on the other, both directions). This program still
// does the actual RTP/RCP talking to the repeater exactly like
// HyteraTransceiver1-10 -- the only thing that changes is WHERE the PTT
// trigger comes from (a COM port input pin, OR VOX off the mic-in audio
// itself) and the addition of a Carrier Detect OUTPUT pin so the other
// computer's software can tell when someone is talking on the repeater.
//
// Serial line directions (standard RS-232 DTE semantics):
// - CTS and DSR are INPUTS on this program's COM port -- they're read,
//   never driven, since the connected device on the far end of the
//   null-modem cable is the one asserting them. That's the PTT trigger
//   in serial mode.
// - RTS and DTR are OUTPUTS on this program's COM port -- this program
//   drives them directly. That's the Carrier Detect signal, raised
//   while someone is talking on the repeater. This runs regardless of
//   whether PTT itself comes from a serial pin or from VOX.
// A null-modem cable normally crosses RTS<->CTS and DTR<->DSR, so
// whichever pin the OTHER computer's software raises as its own PTT
// request arrives here as CTS or DSR, and whichever pin THIS program
// raises as Carrier Detect arrives on the other end as ITS CTS or DSR.
//
// Command line: HyteraConnect {Repeater_IP} {Com_Port} {PTT_Pin_or_VOX} {CD_Pin} {VOX_Level} {VOX_Trigger} {VOX_Delay} {VOX_Inhibit}
//   HyteraConnect 192.168.1.167 COM1 CTS RTS
//   HyteraConnect 192.168.1.167 COM3 DSR DTR
//   HyteraConnect 192.168.1.167 COM1 VOX RTS 250 10 3 1
// PTT_Pin_or_VOX is CTS, DSR, or VOX (default CTS). CD_Pin is RTS or DTR
// (default RTS) and always runs off RX audio activity regardless of
// which PTT mode is chosen. The four VOX_* arguments only apply when
// PTT_Pin_or_VOX is VOX, and each defaults if omitted:
//   VOX_Level   (default 250)  -- amplitude a mic-in sample must exceed
//                                 to count towards a trigger. Same 0-32767
//                                 linear PCM scale the VU meters use --
//                                 250 is a low bar, well above line noise
//                                 but well below normal speech level.
//   VOX_Trigger (default 10)   -- how many individual samples within one
//                                 20ms/160-sample mic buffer must exceed
//                                 VOX_Level for that buffer to count as
//                                 "voice present" (not 10 buffers/frames --
//                                 see VOX_Delay below for the multi-frame
//                                 hang-time side of this).
//   VOX_Delay   (default 3s)   -- hang time: once keyed, PTT stays up and
//                                 keeps refreshing as long as new
//                                 qualifying buffers keep arriving within
//                                 this window; if this much time passes
//                                 with nothing qualifying, PTT releases.
//   VOX_Inhibit (default 1s)   -- minimum time since the last VOX release
//                                 before VOX is allowed to trigger again --
//                                 the anti-runaway-loop guard.
//
// NOTE: unlike HyteraTransceiver, the repeater IP is REQUIRED here (not
// optional with a "wait for the repeater to speak first" fallback) --
// this program's argument positions are fixed by the Com_Port/PTT_Pin/
// CD_Pin slots that follow it, so there's no ambiguous position for it
// to be omitted from.
// ==========================================================
#define LOCAL_IP "192.168.1.136" // Your PC IP
#define PORT_RCP 30009 // Radio Control Port (TX only)
#define PORT_RTP 30012 // Voice/RTP Port (shared RX + TX)
#define TARGET_TALKGROUP 1 // Talkgroup 1
#define CALL_TYPE_GROUP 1 // Group Call

const uint8_t WAKE_CALL_PAYLOAD[] = {0x32, 0x42, 0x00, 0x05, 0x00, 0x00};
const uint8_t KEEP_ALIVE_PAYLOAD[] = {0x32, 0x42, 0x00, 0x02, 0x00, 0x00};

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

// A single lock so the RX VU-meter thread and the serial-control thread
// don't garble each other's console output when they print at the same time.
static CRITICAL_SECTION g_consoleLock;

// Console coloring: RX meter is light green, TX meter is light red.
static HANDLE g_hConsoleOut = NULL;
static WORD g_defaultConsoleAttributes = 0;
#define COLOR_RX (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_TX (FOREGROUND_RED | FOREGROUND_INTENSITY)

static void safe_printf(const char* fmt, ...) {
    EnterCriticalSection(&g_consoleLock);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    LeaveCriticalSection(&g_consoleLock);
}

// Overwrites the CURRENT console row with blanks and returns the cursor
// to column 0 of that same row -- needed before printing anything
// shorter than whatever was on the line before it (e.g. a VU meter bar),
// or the tail end of the old line stays visible past the end of the new
// text. Uses the actual console buffer width via the Windows Console API
// (FillConsoleOutputCharacter + SetConsoleCursorPosition) rather than
// printf-padding with a guessed column count -- padding with more spaces
// than the real buffer width would itself wrap onto a new row and shift
// everything below it down by one line, which is worse than the problem
// this is meant to fix.
static void wipe_console_line(void) {
    EnterCriticalSection(&g_consoleLock);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (g_hConsoleOut && GetConsoleScreenBufferInfo(g_hConsoleOut, &csbi)) {
        COORD lineStart = { 0, csbi.dwCursorPosition.Y };
        DWORD written;
        FillConsoleOutputCharacterA(g_hConsoleOut, ' ', csbi.dwSize.X, lineStart, &written);
        FillConsoleOutputAttribute(g_hConsoleOut, csbi.wAttributes, csbi.dwSize.X, lineStart, &written);
        SetConsoleCursorPosition(g_hConsoleOut, lineStart);
    } else {
        // Fallback if the console API isn't available for some reason --
        // not wrap-safe, but better than nothing.
        printf("\r%110s\r", "");
        fflush(stdout);
    }
    LeaveCriticalSection(&g_consoleLock);
}

static volatile LONG g_app_running = 1;  // Cleared on Esc to shut everything down
static volatile LONG g_transmitting = 0; // 1 while the PTT input pin is high (keyed)

// 1 while someone is currently talking on the repeater (recent RX audio
// activity) -- this is what drives the Carrier Detect output pin. Set by
// RXThreadFunc whenever an audio packet arrives, cleared after
// CD_HANGTIME_MS of no further packets.
static volatile LONG g_carrier_detect = 0;

// Same 300ms threshold already field-confirmed in HyteraParrot as
// comfortably above normal network/OS jitter (observed gaps up to ~100ms
// during unbroken audio) while still catching a genuine end-of-transmission
// promptly -- reused here for the same reason, so Carrier Detect doesn't
// chatter on/off during a single transmission.
#define CD_HANGTIME_MS 300

// ==========================================================
// VOX (new) -- an alternative PTT trigger source to the serial pin,
// driven by the mic-in audio level itself. See the command-line doc
// comment near the top of this file for exactly what each setting means.
// ==========================================================
typedef enum { PTT_MODE_SERIAL, PTT_MODE_VOX } ptt_mode_t;
static ptt_mode_t g_ptt_mode = PTT_MODE_SERIAL;

static int g_vox_level = 250;
static int g_vox_trigger_samples = 10;
static int g_vox_delay_ms = 3000;
static int g_vox_inhibit_ms = 1000;

static volatile LONG g_vox_keyed = 0;             // 1 while VOX currently has PTT engaged
static volatile LONG g_vox_last_loud_tick = 0;    // GetTickCount() of the last qualifying buffer -- drives the VOX_Delay hang timer
static volatile LONG g_vox_last_release_tick = 0; // GetTickCount() of the last VOX de-key -- drives the VOX_Inhibit cooldown

// TX mic volume, adjusted with LEFT (down) / RIGHT (up) -- same 0.5 step,
// same 0.0-5.0 clamp, same edge-triggered key behavior as RX's Up/Down.
static volatile float g_tx_volume_multiplier = 1.0f;

// ==========================================================
// RX SIDE (unchanged from HyteraTransceiver1-10) -- decode incoming
// u-law audio to speaker (i.e. out through the local soundcard's
// line-out, over the analog cable, into the other computer's line-in).
// ==========================================================
#define RX_BUFFER_COUNT 4 // Ring buffers for playback queuing

#define PAYLOAD_OFFSET 28 // Confirmed correct (28, not 29) -- see project notes:
                           // real downlink packets are 508 bytes = 28(header) +
                           // 480(3x160 audio frames), and playback packets are
                           // 188 = 28+160; only 28 divides evenly both ways.

#define NETWORK_BUF_SZ 2048 // Max expected UDP packet size
#define VU_METER_WIDTH 60

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
// logarithmic, so scaling the raw byte directly would distort rather than
// amplify/attenuate it.
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
// decodes u-law to 16-bit PCM, plays it, renders the VU meter, and
// maintains g_carrier_detect for the serial CD output pin. While
// g_transmitting is set, incoming audio is still parsed (so the
// "Radio ID talking" line and Carrier Detect stay accurate) but NOT
// played to the speaker -- half-duplex, avoids hearing your own
// looped-back audio.
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
                safe_printf("\r[Volume Set]: %3.0f%% \n", volume_multiplier * 100.0f);
            }
            if (GetAsyncKeyState(VK_DOWN) & 0x0001) {
                volume_multiplier -= 0.5f;
                if (volume_multiplier < 0.0f) volume_multiplier = 0.0f;
                safe_printf("\r[Volume Set]: %3.0f%% \n", volume_multiplier * 100.0f);
            }
        }

        client_addr_len = sizeof(client_addr);
        int bytes_received = recvfrom(rtp_sock, net_buffer, NETWORK_BUF_SZ, 0,
                                       (struct sockaddr *)&client_addr, &client_addr_len);

        DWORD current_time = GetTickCount();
        if (is_currently_receiving_stream && (current_time - last_audio_packet_time >= 2000)) {
            wipe_console_line();
            safe_printf("Audio Stream Idle.\n");
            is_currently_receiving_stream = 0;
            is_first_radio_id_parsed = 0;
        }

        // Carrier Detect uses its own, much shorter hang time than the
        // console's "Audio Stream Idle" message above -- see CD_HANGTIME_MS.
        if (InterlockedCompareExchange(&g_carrier_detect, 0, 0) && (current_time - last_audio_packet_time >= CD_HANGTIME_MS)) {
            InterlockedExchange(&g_carrier_detect, 0);
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
                safe_printf("[DMR] Radio ID: %u talking to Group %u\n", current_radio_id, parsed_group);
                is_currently_receiving_stream = 1;
            }
        }

        if (bytes_received <= PAYLOAD_OFFSET) continue;

        int audio_payload_len = bytes_received - PAYLOAD_OFFSET;
        uint8_t *ulaw_ptr = (uint8_t *)(net_buffer + PAYLOAD_OFFSET);

        last_audio_packet_time = GetTickCount();
        is_currently_receiving_stream = 1;
        InterlockedExchange(&g_carrier_detect, 1); // Real audio just arrived -- assert CD
        audio_packet_counter++;

        // Skip actual playback while transmitting -- half-duplex.
        // Still counted above so the VU meter / idle detection / CD stay in sync.
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
                   should_play ? " " : "[MUTED-TX]");
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
// TX SIDE (unchanged from HyteraTransceiver1-10) -- microphone input
// (i.e. in from the local soundcard's line-in/mic-in, fed by the other
// computer's line-out over the second analog cable) -> RTP.
// ==========================================================
#define WAVE_FORMAT_MULAW_TAG 7
#define AUDIO_BUFFER_BYTES 160 // 8000Hz * 0.020s * 1 byte/sample = one 20ms RTP frame
#define NUM_AUDIO_BUFFERS 8

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
    uint8_t hytera_pad[16];
    uint8_t voice_payload[AUDIO_BUFFER_BYTES];
} rtp_packet_t;
#pragma pack(pop)

static uint8_t rcp_sequence_counter = 0;
static uint16_t rtp_sequence_counter = 0;
static uint32_t rtp_timestamp_counter = 0;

static HWAVEIN g_hWaveIn = NULL;
static WAVEHDR g_waveHeaders[NUM_AUDIO_BUFFERS];
static uint8_t g_audioBuffers[NUM_AUDIO_BUFFERS][AUDIO_BUFFER_BYTES];
static HANDLE g_hDataEvent = NULL;
static HANDLE g_hStopEvent = NULL;
static volatile LONG g_stop_requested = 0;

uint8_t get_next_rcp_seq() { return rcp_sequence_counter++; }

void send_call_setup(SOCKET sock, struct sockaddr_in* addr, uint8_t call_type, uint32_t target_id) {
    uint8_t packet[sizeof(CALL_SETUP_TEMPLATE)];
    memcpy(packet, CALL_SETUP_TEMPLATE, sizeof(CALL_SETUP_TEMPLATE));
    packet[5] = get_next_rcp_seq();
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
    else { packet[12] = 0x00; packet[13] = 0xEC; }
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

// Shared key-up/key-down actions -- used by BOTH PTT sources (serial pin
// and VOX) so there's exactly one place that actually talks to the
// repeater, regardless of what triggered the request.
static void engage_ptt(SOCKET rcp_sock, struct sockaddr_in* remote_rcp_addr, const char* reason) {
    safe_printf("[RCP] Re-establishing Call Setup for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);
    safe_printf("[RCP] PTT Key-Up (%s)...\n", reason);
    send_ptt_command(rcp_sock, remote_rcp_addr, 1);
    InterlockedExchange(&g_transmitting, 1);
}

static void release_ptt(SOCKET rcp_sock, struct sockaddr_in* remote_rcp_addr, const char* reason) {
    InterlockedExchange(&g_transmitting, 0);
    wipe_console_line(); // Wipe the remaining TX VU meter line
    safe_printf("[RCP] PTT De-Key (%s)...\n", reason);
    send_ptt_command(rcp_sock, remote_rcp_addr, 0);
}

// Analyzes one already-decoded 20ms mic-in buffer for VOX and drives
// g_transmitting accordingly. Called for EVERY captured buffer while in
// VOX mode, regardless of whether PTT is currently engaged -- that's what
// lets it both detect a fresh trigger and refresh the hang timer while
// already keyed.
static void process_vox_buffer(const int16_t* decoded, DWORD sampleCount, SOCKET rcp_sock, struct sockaddr_in* remote_rcp_addr) {
    int over_count = 0;
    for (DWORD s = 0; s < sampleCount; s++) {
        int amp = decoded[s];
        if (amp < 0) amp = -amp;
        if (amp > g_vox_level) over_count++;
    }

    BOOL loud_enough = (over_count >= g_vox_trigger_samples);
    DWORD now = GetTickCount();

    if (loud_enough) {
        if (!InterlockedCompareExchange(&g_vox_keyed, 0, 0)) {
            // Not currently VOX-keyed -- only start a new transmission if
            // VOX_Inhibit has elapsed since the last VOX release. This,
            // together with the hang-time release logic below (releasing
            // on a timer rather than instantly on quiet), is what stops
            // VOX from chattering or looping on its own tail/residual
            // line noise right after de-keying.
            DWORD since_release = now - (DWORD)g_vox_last_release_tick;
            if (since_release >= (DWORD)g_vox_inhibit_ms) {
                InterlockedExchange(&g_vox_keyed, 1);
                InterlockedExchange(&g_vox_last_loud_tick, (LONG)now);
                engage_ptt(rcp_sock, remote_rcp_addr, "VOX");
            }
            // else: still inhibited -- this loud buffer is ignored outright
        } else {
            InterlockedExchange(&g_vox_last_loud_tick, (LONG)now); // Refresh the hang timer
        }
    } else {
        if (InterlockedCompareExchange(&g_vox_keyed, 0, 0)) {
            DWORD since_last_loud = now - (DWORD)g_vox_last_loud_tick;
            if (since_last_loud >= (DWORD)g_vox_delay_ms) {
                InterlockedExchange(&g_vox_keyed, 0);
                InterlockedExchange(&g_vox_last_release_tick, (LONG)now);
                release_ptt(rcp_sock, remote_rcp_addr, "VOX hang time expired");
            }
        }
    }
}

VOID CALLBACK SendKeepaliveCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer) {
    keepalive_ctx_t *ctx = (keepalive_ctx_t*)Context;
    sendto(ctx->socket, (const char*)KEEP_ALIVE_PAYLOAD, sizeof(KEEP_ALIVE_PAYLOAD), 0,
           (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

int start_mic_capture() {
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_MULAW_TAG;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 8000;
    wfx.nAvgBytesPerSec = 8000;
    wfx.nBlockAlign = 1;
    wfx.wBitsPerSample = 8;
    wfx.cbSize = 0;

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
        g_waveHeaders[i].lpData = (LPSTR)g_audioBuffers[i];
        g_waveHeaders[i].dwBufferLength = AUDIO_BUFFER_BYTES;
        waveInPrepareHeader(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
    }

    res = waveInStart(g_hWaveIn);
    if (res != MMSYSERR_NOERROR) {
        printf("[ERROR] waveInStart failed (code %d).\n", res);
        return 0;
    }

    printf("[MIC] Microphone-in capture started (G.711 u-law, 8000Hz, mono).\n");
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
    SOCKET rcp_sock;
    struct sockaddr_in* remote_rcp_addr;
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

            DWORD bytesRecorded = g_waveHeaders[i].dwBytesRecorded;
            if (bytesRecorded > AUDIO_BUFFER_BYTES) bytesRecorded = AUDIO_BUFFER_BYTES;
            uint8_t *mic_bytes = (uint8_t*)g_waveHeaders[i].lpData;

            // Decode once per buffer -- used both for VOX analysis below
            // (which must run on every buffer, not just while already
            // transmitting) and for the gain/encode/send path further down.
            int16_t decoded[AUDIO_BUFFER_BYTES];
            for (DWORD s = 0; s < bytesRecorded; s++) decoded[s] = ulaw_to_pcm_lut[mic_bytes[s]];

            if (g_ptt_mode == PTT_MODE_VOX) {
                process_vox_buffer(decoded, bytesRecorded, ctx->rcp_sock, ctx->remote_rcp_addr);
            }

            if (InterlockedCompareExchange(&g_transmitting, 0, 0)) {
                rtp_packet_t audio_pkt;
                memset(&audio_pkt, 0, sizeof(audio_pkt));
                audio_pkt.fixed_marker = htons(0x9000);
                audio_pkt.seq_num = htons(rtp_sequence_counter++);
                audio_pkt.timestamp = htonl(rtp_timestamp_counter);
                audio_pkt.ssrc = 0;
                audio_pkt.hytera_pad[1] = 0x15;
                audio_pkt.hytera_pad[3] = 0x03;

                // Apply TX gain in the linear PCM domain (u-law is logarithmic,
                // so scaling the raw byte directly would distort rather than
                // amplify/attenuate it) -- scale+clamp the already-decoded
                // samples, then re-encode.
                //
                // FIX (1-4): when gain is at its default 1.0x, skip the
                // re-encode and copy the mic driver's own u-law bytes straight
                // through instead. decoded[] is still computed unconditionally
                // above (VOX needs it regardless of TX gain), but re-encoding
                // it back to u-law when nothing needs scaling stacks a second
                // lossy quantization pass on top of the mic driver's own u-law
                // encode -- the same "tiny bit fuzzy" TX audio bug fixed in
                // HyteraTransceiver1-12. Comparing tx_gain to 1.0f directly is
                // safe here specifically because g_tx_volume_multiplier only
                // ever starts at exactly 1.0f and moves in exact 0.5f steps
                // (both exactly representable in binary float), so it lands
                // back on exact 1.0f whenever TX gain is returned to default.
                float tx_gain = g_tx_volume_multiplier;

                if (tx_gain == 1.0f) {
                    memcpy(audio_pkt.voice_payload, mic_bytes, bytesRecorded);
                    for (DWORD s = 0; s < bytesRecorded; s++) {
                        tx_accumulated_amplitude_sum += abs(decoded[s]);
                        tx_total_samples_accumulated++;
                    }
                } else {
                    for (DWORD s = 0; s < bytesRecorded; s++) {
                        float amplified = (float)decoded[s] * tx_gain;
                        int16_t clamped;
                        if (amplified > 32767.0f) clamped = 32767;
                        else if (amplified < -32768.0f) clamped = -32768;
                        else clamped = (int16_t)amplified;
                        audio_pkt.voice_payload[s] = linear_to_ulaw(clamped);
                        tx_accumulated_amplitude_sum += abs(clamped);
                        tx_total_samples_accumulated++;
                    }
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
                    printf("] TX Level: %4.0f | TX Gain: %3.0f%% ", average_amplitude, tx_gain * 100.0f);
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

    // If VOX still has PTT engaged when shutting down, release it cleanly
    // rather than leaving the repeater thinking we're still keyed.
    if (g_ptt_mode == PTT_MODE_VOX && InterlockedCompareExchange(&g_vox_keyed, 0, 0)) {
        InterlockedExchange(&g_vox_keyed, 0);
        release_ptt(ctx->rcp_sock, ctx->remote_rcp_addr, "VOX shutdown");
    }

    return 0;
}

// ==========================================================
// SERIAL CONTROL -- new for HyteraConnect. Replaces HyteraTransceiver's
// Caps-Lock PTT with a COM port's CTS/DSR input, and adds an RTS/DTR
// Carrier Detect output driven by g_carrier_detect (maintained by
// RXThreadFunc above).
// ==========================================================
typedef enum { PTT_PIN_CTS, PTT_PIN_DSR } ptt_pin_t;
typedef enum { CD_PIN_RTS, CD_PIN_DTR } cd_pin_t;

static const char* ptt_pin_name(ptt_pin_t pin) { return (pin == PTT_PIN_CTS) ? "CTS" : "DSR"; }
static const char* cd_pin_name(cd_pin_t pin) { return (pin == CD_PIN_RTS) ? "RTS" : "DTR"; }

// Configures the COM port for pure control-line use -- no data bytes are
// ever sent or received over it, only the modem status lines (CTS/DSR in,
// RTS/DTR out), so baud/parity/etc genuinely don't matter beyond keeping
// Windows happy with a valid DCB. The two fields that DO matter:
// - fRtsControl/fDtrControl = RTS_CONTROL_ENABLE/DTR_CONTROL_ENABLE --
//   this is the standard trick (used by e.g. pyserial on Windows) that
//   lets EscapeCommFunction(SETRTS/CLRRTS/SETDTR/CLRDTR) actually
//   override the line manually afterwards. HANDSHAKE or TOGGLE modes
//   would fight with our manual control instead.
// - fOutxCtsFlow/fOutxDsrFlow/fDsrSensitivity = FALSE -- so Windows
//   doesn't try to use CTS/DSR for real flow control on a port where
//   we're repurposing them as plain logic-level inputs.
static BOOL configure_serial_port(HANDLE hComm) {
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hComm, &dcb)) return FALSE;

    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(hComm, &dcb)) return FALSE;

    // We never call ReadFile/WriteFile on this handle at all -- only
    // GetCommModemStatus/EscapeCommFunction -- so these timeouts don't
    // really matter, but setting sane non-blocking-ish values costs nothing.
    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hComm, &timeouts);

    return TRUE;
}

static BOOL is_ptt_pin_high(HANDLE hComm, ptt_pin_t pin) {
    DWORD modemStat = 0;
    if (!GetCommModemStatus(hComm, &modemStat)) return FALSE;
    return (pin == PTT_PIN_CTS) ? ((modemStat & MS_CTS_ON) != 0) : ((modemStat & MS_DSR_ON) != 0);
}

static void set_cd_pin_state(HANDLE hComm, cd_pin_t pin, BOOL high) {
    if (pin == CD_PIN_RTS) EscapeCommFunction(hComm, high ? SETRTS : CLRRTS);
    else EscapeCommFunction(hComm, high ? SETDTR : CLRDTR);
}

typedef struct {
    HANDLE hComm;
    ptt_pin_t ptt_pin;
    cd_pin_t cd_pin;
    SOCKET rcp_sock;
    struct sockaddr_in* remote_rcp_addr;
} serial_ctx_t;

// Runs for the life of the program on its own thread: polls the PTT input
// pin every 10ms (same cadence HyteraTransceiver polled Caps Lock/spacebar
// at) and keys/de-keys the repeater on its transitions, and separately
// drives the CD output pin from g_carrier_detect, only touching the pin
// via EscapeCommFunction when its state actually changes.
DWORD WINAPI SerialControlThread(LPVOID lpParam) {
    serial_ctx_t* ctx = (serial_ctx_t*)lpParam;
    int wasKeyed = 0;
    BOOL cdOutputCurrentlyHigh = FALSE;

    set_cd_pin_state(ctx->hComm, ctx->cd_pin, FALSE); // Start deasserted

    while (InterlockedCompareExchange(&g_app_running, 0, 0)) {

        // Serial-pin PTT polling only applies in serial mode -- in VOX
        // mode, g_transmitting is driven entirely by process_vox_buffer()
        // over in AudioSenderThread instead, and this pin is never read
        // for that purpose (though it's still a plain unused input if
        // left wired up).
        if (g_ptt_mode == PTT_MODE_SERIAL) {
            BOOL pttPinHigh = is_ptt_pin_high(ctx->hComm, ctx->ptt_pin);
            if (pttPinHigh && !wasKeyed) {
                safe_printf("[SERIAL] %s went HIGH -- keying up.\n", ptt_pin_name(ctx->ptt_pin));
                engage_ptt(ctx->rcp_sock, ctx->remote_rcp_addr, "serial PTT pin");
                wasKeyed = 1;
            } else if (!pttPinHigh && wasKeyed) {
                safe_printf("[SERIAL] %s went LOW -- de-keying.\n", ptt_pin_name(ctx->ptt_pin));
                release_ptt(ctx->rcp_sock, ctx->remote_rcp_addr, "serial PTT pin");
                wasKeyed = 0;
            }
        }

        // CD output driving runs regardless of PTT mode -- it reflects RX
        // audio activity from the repeater, which happens independently
        // of whether TX is triggered by a serial pin or by VOX.
        BOOL cdShouldBeHigh = (InterlockedCompareExchange(&g_carrier_detect, 0, 0) != 0);
        if (cdShouldBeHigh != cdOutputCurrentlyHigh) {
            set_cd_pin_state(ctx->hComm, ctx->cd_pin, cdShouldBeHigh);
            // The RX VU meter line has no trailing newline (by design, so
            // it can be redrawn in place) -- wipe it first so this message
            // lands on a clean line instead of getting appended onto
            // whatever the meter last drew.
            wipe_console_line();
            safe_printf("[SERIAL] %s set %s (Carrier Detect %s).\n",
                        cd_pin_name(ctx->cd_pin), cdShouldBeHigh ? "HIGH" : "LOW",
                        cdShouldBeHigh ? "asserted" : "cleared");
            cdOutputCurrentlyHigh = cdShouldBeHigh;
        }

        Sleep(10);
    }

    if (g_ptt_mode == PTT_MODE_SERIAL && wasKeyed) {
        release_ptt(ctx->rcp_sock, ctx->remote_rcp_addr, "serial shutdown");
    }
    set_cd_pin_state(ctx->hComm, ctx->cd_pin, FALSE); // Deassert CD on shutdown

    return 0;
}

// ==========================================================
// MAIN -- brings RX, TX, and the serial control thread up together
// ==========================================================
int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <Repeater_IP> <Com_Port> [PTT_Pin_or_VOX] [CD_Pin] [VOX_Level] [VOX_Trigger] [VOX_Delay] [VOX_Inhibit]\n", argv[0]);
        printf("  PTT_Pin_or_VOX: CTS, DSR, or VOX (default CTS)\n");
        printf("  CD_Pin:  RTS or DTR (default RTS)\n");
        printf("  VOX_Level (default 250), VOX_Trigger (default 10 samples),\n");
        printf("  VOX_Delay (default 3s), VOX_Inhibit (default 1s) -- only used when PTT mode is VOX\n");
        printf("Examples:\n");
        printf("  %s 192.168.1.167 COM1 CTS RTS\n", argv[0]);
        printf("  %s 192.168.1.167 COM3 DSR DTR\n", argv[0]);
        printf("  %s 192.168.1.167 COM1 VOX RTS 250 10 3 1\n", argv[0]);
        return -1;
    }

    // --- Repeater IP (required -- see note near the top of this file) ---
    struct in_addr testAddr;
    if (inet_pton(AF_INET, argv[1], &testAddr) != 1) {
        printf("[ERROR] '%s' is not a valid IPv4 address.\n", argv[1]);
        return -1;
    }
    char repeater_ip_str[INET_ADDRSTRLEN];
    strncpy(repeater_ip_str, argv[1], sizeof(repeater_ip_str) - 1);
    repeater_ip_str[sizeof(repeater_ip_str) - 1] = '\0';

    // --- PTT_Pin_or_VOX (optional, default CTS) / CD_Pin (optional, default RTS) ---
    const char* ptt_arg = (argc >= 4) ? argv[3] : "CTS";
    const char* cd_pin_arg = (argc >= 5) ? argv[4] : "RTS";

    ptt_pin_t ptt_pin = PTT_PIN_CTS; // only meaningful when g_ptt_mode == PTT_MODE_SERIAL
    if (_stricmp(ptt_arg, "VOX") == 0) {
        g_ptt_mode = PTT_MODE_VOX;
    } else if (_stricmp(ptt_arg, "CTS") == 0) {
        g_ptt_mode = PTT_MODE_SERIAL; ptt_pin = PTT_PIN_CTS;
    } else if (_stricmp(ptt_arg, "DSR") == 0) {
        g_ptt_mode = PTT_MODE_SERIAL; ptt_pin = PTT_PIN_DSR;
    } else {
        printf("[ERROR] PTT_Pin_or_VOX must be CTS, DSR, or VOX (got '%s').\n", ptt_arg);
        return -1;
    }

    cd_pin_t cd_pin;
    if (_stricmp(cd_pin_arg, "RTS") == 0) cd_pin = CD_PIN_RTS;
    else if (_stricmp(cd_pin_arg, "DTR") == 0) cd_pin = CD_PIN_DTR;
    else {
        printf("[ERROR] CD_Pin must be RTS or DTR (got '%s').\n", cd_pin_arg);
        return -1;
    }

    // --- VOX_Level / VOX_Trigger / VOX_Delay / VOX_Inhibit (all optional, only used in VOX mode) ---
    if (argc >= 6) g_vox_level = atoi(argv[5]);
    if (argc >= 7) g_vox_trigger_samples = atoi(argv[6]);
    if (argc >= 8) g_vox_delay_ms = (int)(atof(argv[7]) * 1000.0);
    if (argc >= 9) g_vox_inhibit_ms = (int)(atof(argv[8]) * 1000.0);

    // Sanity-clamp rather than hard-fail on out-of-range values.
    if (g_vox_level < 0) g_vox_level = 0;
    if (g_vox_level > 32767) g_vox_level = 32767;
    if (g_vox_trigger_samples < 1) g_vox_trigger_samples = 1;
    if (g_vox_trigger_samples > AUDIO_BUFFER_BYTES) g_vox_trigger_samples = AUDIO_BUFFER_BYTES;
    if (g_vox_delay_ms < 0) g_vox_delay_ms = 0;
    if (g_vox_inhibit_ms < 0) g_vox_inhibit_ms = 0;

    // --- Open and configure the COM port ---
    char comPath[64];
    snprintf(comPath, sizeof(comPath), "\\\\.\\%s", argv[2]);

    HANDLE hComm = CreateFileA(comPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hComm == INVALID_HANDLE_VALUE) {
        printf("[ERROR] Could not open %s (GetLastError=%lu). Is it the right port, and not already in use?\n",
               argv[2], GetLastError());
        return -1;
    }
    if (!configure_serial_port(hComm)) {
        printf("[ERROR] Could not configure %s (GetLastError=%lu).\n", argv[2], GetLastError());
        CloseHandle(hComm);
        return -1;
    }

    printf("[AUTHOR] HyteraConnect made by Rob Thompson 2E0RPT...\n");
    if (g_ptt_mode == PTT_MODE_VOX) {
        printf("[SYSTEM] Repeater: %s | Serial: %s | PTT: VOX (Level=%d, Trigger=%d samples, Delay=%.1fs, Inhibit=%.1fs) | CD output: %s\n",
               repeater_ip_str, argv[2], g_vox_level, g_vox_trigger_samples,
               g_vox_delay_ms / 1000.0, g_vox_inhibit_ms / 1000.0, cd_pin_name(cd_pin));
    } else {
        printf("[SYSTEM] Repeater: %s | Serial: %s | PTT input: %s | CD output: %s\n",
               repeater_ip_str, argv[2], ptt_pin_name(ptt_pin), cd_pin_name(cd_pin));
    }

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
        CloseHandle(hComm);
        timeEndPeriod(1);
        return -1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock initialization failed.\n");
        shutdown_playback_device();
        CloseHandle(hComm);
        timeEndPeriod(1);
        return -1;
    }

    SOCKET rcp_sock, rtp_sock;
    struct sockaddr_in local_rcp_addr, local_rtp_addr;
    struct sockaddr_in remote_rcp_addr, remote_rtp_addr;

    if ((rcp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET ||
        (rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        WSACleanup(); shutdown_playback_device(); CloseHandle(hComm); timeEndPeriod(1);
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

    memset(&local_rtp_addr, 0, sizeof(local_rtp_addr));
    local_rtp_addr.sin_family = AF_INET;
    local_rtp_addr.sin_port = htons(PORT_RTP);
    local_rtp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(rcp_sock, (struct sockaddr*)&local_rcp_addr, sizeof(local_rcp_addr)) == SOCKET_ERROR ||
        bind(rtp_sock, (struct sockaddr*)&local_rtp_addr, sizeof(local_rtp_addr)) == SOCKET_ERROR) {
        printf("[ERROR] Socket port bindings failed.\n");
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup(); shutdown_playback_device(); CloseHandle(hComm); timeEndPeriod(1);
        return -1;
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
    printf("[RX] Use UP/DOWN keys to adjust RX gain.\n");
    printf("[TX] Use LEFT/RIGHT keys to adjust TX gain.\n");

    // Initial Call Setup (also re-sent before every subsequent key-up, since
    // the repeater tears the call down as soon as PTT de-keys)
    printf("[RCP] Sending Call Setup Envelope for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, &remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);

    if (!start_mic_capture()) {
        printf("[ERROR] Could not start microphone-in capture.\n");
        InterlockedExchange(&g_app_running, 0);
        WaitForSingleObject(hRxThread, 2000);
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup(); shutdown_playback_device(); CloseHandle(hComm); timeEndPeriod(1);
        return -1;
    }

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    sender_thread_ctx_t sender_ctx = { rtp_sock, &remote_rtp_addr, rcp_sock, &remote_rcp_addr };
    HANDLE hSenderThread = CreateThread(NULL, 0, AudioSenderThread, &sender_ctx, 0, NULL);

    serial_ctx_t serial_ctx = { hComm, ptt_pin, cd_pin, rcp_sock, &remote_rcp_addr };
    HANDLE hSerialThread = CreateThread(NULL, 0, SerialControlThread, &serial_ctx, 0, NULL);

    if (g_ptt_mode == PTT_MODE_VOX) {
        printf("[PTT] Ready. VOX-triggered transmit is active. %s will go HIGH (Carrier Detect) whenever\n",
               cd_pin_name(cd_pin));
    } else {
        printf("[PTT] Ready. %s HIGH = transmit, LOW = idle. %s will go HIGH (Carrier Detect) whenever\n",
               ptt_pin_name(ptt_pin), cd_pin_name(cd_pin));
    }
    printf("      someone is talking on the repeater. Press ESC to quit.\n");

    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        // --- TX volume hotkeys (same edge-triggered pattern as RX's Up/Down) ---
        if (GetForegroundWindow() == GetConsoleWindow()) {
            if (GetAsyncKeyState(VK_RIGHT) & 0x0001) {
                float v = g_tx_volume_multiplier + 0.5f;
                if (v > 5.0f) v = 5.0f;
                g_tx_volume_multiplier = v;
                safe_printf("\r[TX Volume Set]: %3.0f%% \n", v * 100.0f);
            }
            if (GetAsyncKeyState(VK_LEFT) & 0x0001) {
                float v = g_tx_volume_multiplier - 0.5f;
                if (v < 0.0f) v = 0.0f;
                g_tx_volume_multiplier = v;
                safe_printf("\r[TX Volume Set]: %3.0f%% \n", v * 100.0f);
            }
        }

        Sleep(10);
    }

    // Shut down serial control thread (also de-keys/deasserts CD if needed, see its own end-of-loop handling)
    InterlockedExchange(&g_app_running, 0);
    if (hSerialThread) { WaitForSingleObject(hSerialThread, 2000); CloseHandle(hSerialThread); }

    // Shut down TX sender thread
    InterlockedExchange(&g_stop_requested, 1);
    SetEvent(g_hStopEvent);
    if (hSenderThread) { WaitForSingleObject(hSenderThread, 2000); CloseHandle(hSenderThread); }
    stop_mic_capture();

    // Shut down RX thread
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
    CloseHandle(hComm);
    timeEndPeriod(1);
    DeleteCriticalSection(&g_consoleLock);

    printf("[DONE] HyteraConnect session completed cleanly.\n");
    return 0;
}
