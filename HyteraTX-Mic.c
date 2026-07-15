#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> 
#include <time.h>   

// --- Windows-Specific Network and System Headers ---
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>   // Native Windows Multimedia Timer API AND waveIn recording API
#pragma comment(lib, "ws2_32.lib") 
#pragma comment(lib, "winmm.lib")  // Links multimedia scheduling + waveIn extensions

#define close closesocket

// --- Configuration Constants ---
#define LOCAL_IP             "192.168.1.136" // Your PC IP
#define REPEATER_IP          "192.168.1.167" // Target Repeater IP
#define PORT_RCP             30009           // Radio Call Control Slot 1 Port
#define PORT_RTP             30012           // Radio Voice Service Slot 1 Port

// Fleet Map Profile Configuration
#define TARGET_TALKGROUP     1               // Talkgroup 1
#define CALL_TYPE_GROUP      1               // Group Call

// --- Audio Capture Configuration ---
// Confirmed from the fmt chunk of input.raw: WAVE_FORMAT_MULAW (tag 7),
// mono, 8000 Hz, 8 bits/sample, block align 1 byte/sample.
#define WAVE_FORMAT_MULAW_TAG 7
#define AUDIO_BUFFER_BYTES    160   // 8000 samples/sec * 0.020s * 1 byte/sample = 160 bytes = one 20ms RTP frame
#define NUM_AUDIO_BUFFERS     8     // Several buffers in flight so a slow sendto() never causes a gap

// --- Hytera Control Protocol Templates ---
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

// --- Shared Context for Threadpool Timers ---
typedef struct {
    SOCKET socket;
    struct sockaddr_in target_addr;
} keepalive_ctx_t;

// --- 28-Byte RTP Audio Header Struct Layout ---
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

static uint8_t rcp_sequence_counter = 0;
static uint16_t rtp_sequence_counter = 0;
static uint32_t rtp_timestamp_counter = 0;

// --- Live Microphone Capture State ---
static HWAVEIN   g_hWaveIn = NULL;
static WAVEHDR   g_waveHeaders[NUM_AUDIO_BUFFERS];
static uint8_t   g_audioBuffers[NUM_AUDIO_BUFFERS][AUDIO_BUFFER_BYTES];
static HANDLE    g_hDataEvent = NULL;   // Signaled by waveIn driver whenever a buffer is filled
static HANDLE    g_hStopEvent = NULL;   // Signaled by main thread (Enter key) to end the sender thread
static volatile LONG g_stop_requested = 0;

uint8_t get_next_rcp_seq() {
    return rcp_sequence_counter++;
}

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
    
    if (turn_on) {
        packet[12] = 0x01;
        packet[13] = 0xEB;
    } else {
        packet[12] = 0x00;
        packet[13] = 0xEC;
    }
    
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

// Background Keepalive Callback Thread
VOID CALLBACK SendKeepaliveCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer) {
    keepalive_ctx_t *ctx = (keepalive_ctx_t*)Context;
    sendto(ctx->socket, (const char*)KEEP_ALIVE_PAYLOAD, sizeof(KEEP_ALIVE_PAYLOAD), 0, 
           (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

// ==========================================================
// Live microphone capture setup
// ==========================================================
// Opens the default recording device directly in G.711 mu-law,
// 8000Hz, mono, 8-bit -- the exact format the repeater expects.
// Windows' built-in "Microsoft CCITT G.711" ACM codec handles the
// conversion from whatever the physical mic natively records, so no
// manual encoding step is required here.
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
        printf("       Check Windows Sound settings for the correct default input device.\n");
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
    waveInReset(g_hWaveIn); // Marks all outstanding buffers as done and returns them
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        waveInUnprepareHeader(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
    }
    waveInClose(g_hWaveIn);
    g_hWaveIn = NULL;
}

// ==========================================================
// Audio sender thread: wakes up whenever the driver finishes filling
// a buffer, immediately wraps that buffer in an RTP packet and sends
// it, then re-queues the same buffer for the next 20ms of audio.
// This replaces the old file-read + precise_sleep_20ms() loop -- the
// sound card itself now provides the 20ms pacing.
// ==========================================================
typedef struct {
    SOCKET rtp_sock;
    struct sockaddr_in* remote_rtp_addr;
} sender_thread_ctx_t;

DWORD WINAPI AudioSenderThread(LPVOID lpParam) {
    sender_thread_ctx_t* ctx = (sender_thread_ctx_t*)lpParam;
    HANDLE waitHandles[2] = { g_hDataEvent, g_hStopEvent };

    while (1) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1) {
            // Stop event signaled -- flush any remaining done buffers, then exit
        }

        for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
            if (!(g_waveHeaders[i].dwFlags & WHDR_DONE)) {
                continue;
            }

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
            memcpy(audio_pkt.voice_payload, g_waveHeaders[i].lpData, bytesRecorded);
            if (bytesRecorded < AUDIO_BUFFER_BYTES) {
                memset(audio_pkt.voice_payload + bytesRecorded, 0xFF, AUDIO_BUFFER_BYTES - bytesRecorded);
            }

            sendto(ctx->rtp_sock, (const char*)&audio_pkt, sizeof(audio_pkt), 0,
                   (struct sockaddr*)ctx->remote_rtp_addr, sizeof(*ctx->remote_rtp_addr));

            rtp_timestamp_counter += AUDIO_BUFFER_BYTES;

            // Buffer processed -- hand it straight back to the driver for the next chunk
            g_waveHeaders[i].dwFlags &= ~WHDR_DONE;
            g_waveHeaders[i].dwBytesRecorded = 0;
            waveInAddBuffer(g_hWaveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
        }

        if (InterlockedCompareExchange(&g_stop_requested, 0, 0)) {
            break;
        }
    }
    return 0;
}

int main() {
    // Force the Windows OS Kernel scheduler to utilize a 1ms timing tick rate
    timeBeginPeriod(1);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock initialization failed.\n");
        timeEndPeriod(1);
        return -1;
    }

    SOCKET rcp_sock, rtp_sock;
    struct sockaddr_in local_rcp_addr, local_rtp_addr;
    struct sockaddr_in remote_rcp_addr, remote_rtp_addr;

    if ((rcp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET || 
        (rtp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        WSACleanup(); timeEndPeriod(1);
        return -1;
    }

    memset(&local_rcp_addr, 0, sizeof(local_rcp_addr));
    local_rcp_addr.sin_family = AF_INET;
    local_rcp_addr.sin_port = htons(PORT_RCP);
    inet_pton(AF_INET, LOCAL_IP, &local_rcp_addr.sin_addr);

    memset(&local_rtp_addr, 0, sizeof(local_rtp_addr));
    local_rtp_addr.sin_family = AF_INET;
    local_rtp_addr.sin_port = htons(PORT_RTP);
    inet_pton(AF_INET, LOCAL_IP, &local_rtp_addr.sin_addr);

    if (bind(rcp_sock, (struct sockaddr*)&local_rcp_addr, sizeof(local_rcp_addr)) == SOCKET_ERROR ||
        bind(rtp_sock, (struct sockaddr*)&local_rtp_addr, sizeof(local_rtp_addr)) == SOCKET_ERROR) {
        printf("[ERROR] Socket port bindings failed.\n");
        close(rcp_sock); close(rtp_sock); WSACleanup(); timeEndPeriod(1);
        return -1;
    }

    memset(&remote_rcp_addr, 0, sizeof(remote_rcp_addr));
    remote_rcp_addr.sin_family = AF_INET;
    remote_rcp_addr.sin_port = htons(PORT_RCP);
    inet_pton(AF_INET, REPEATER_IP, &remote_rcp_addr.sin_addr);

    memset(&remote_rtp_addr, 0, sizeof(remote_rtp_addr));
    remote_rtp_addr.sin_family = AF_INET;
    remote_rtp_addr.sin_port = htons(PORT_RTP);
    inet_pton(AF_INET, REPEATER_IP, &remote_rtp_addr.sin_addr);

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
        printf("[SYSTEM] Asynchronous 5-second keepalives armed.\n");
    }

    printf("[SYSTEM] Holding link stabilization window for 4 seconds...\n");
    Sleep(4000); 

    // ==========================================
    // PHASE 1: TRANSMIT CALL SETUP & PTT KEY-UP
    // ==========================================
    printf("[RCP] Sending Call Setup Envelope for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, &remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);

    printf("[RCP] Broadcasting PTT Key-Up Command...\n");
    send_ptt_command(rcp_sock, &remote_rcp_addr, 1);

    printf("[SYSTEM] Holding 2-second amplifier pre-roll stabilization pause...\n");
    Sleep(2000); 

    // ==========================================
    // PHASE 2: LIVE MICROPHONE AUDIO STREAMING
    // ==========================================
    if (!start_mic_capture()) {
        printf("[ERROR] Could not start microphone capture. De-keying and exiting.\n");
        send_ptt_command(rcp_sock, &remote_rcp_addr, 0);
        close(rcp_sock); close(rtp_sock); WSACleanup(); timeEndPeriod(1);
        return -1;
    }

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    sender_thread_ctx_t sender_ctx = { rtp_sock, &remote_rtp_addr };
    HANDLE hSenderThread = CreateThread(NULL, 0, AudioSenderThread, &sender_ctx, 0, NULL);

    printf("[RTP] Transmitting live microphone audio. Press ENTER to stop transmitting...\n");
    getchar();

    // Signal the sender thread to finish and wait for it to exit cleanly
    InterlockedExchange(&g_stop_requested, 1);
    SetEvent(g_hStopEvent);
    if (hSenderThread) {
        WaitForSingleObject(hSenderThread, 2000);
        CloseHandle(hSenderThread);
    }

    stop_mic_capture();
    printf("[RTP] Microphone stream stopped.\n");

    // ==========================================
    // PHASE 3: TRANSMIT PTT DE-KEY & CLEANUP
    // ==========================================
    printf("[RCP] Broadcasting PTT De-Key Command...\n");
    send_ptt_command(rcp_sock, &remote_rcp_addr, 0);

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

    close(rcp_sock);
    close(rtp_sock);
    WSACleanup();
    timeEndPeriod(1);
    printf("[DONE] Session completed cleanly.\n");
    return 0;
}
