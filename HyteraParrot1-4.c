#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Enable Winsock2 specifications for Windows Vista/7/10+
#endif
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h> // Needed for timeBeginPeriod/timeEndPeriod (precise Sleep() timing)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ==========================================================
// HyteraParrot -- field test tool.
//
// Listens for a station transmitting through the repeater, buffers their
// audio, and once the transmission has been truly silent for 5 seconds,
// plays it straight back to them through the repeater -- lets an operator
// key up, walk to a test location, and hear exactly what the repeater
// actually received from them.
//
// The 5 second wait (not just "de-key detected") is deliberate: a weak
// or fading signal can drop out for a moment and come back. If that
// happens within the 5 second window, the new audio is appended to the
// SAME recording rather than starting a fresh one or playing back a
// truncated clip -- with the correct amount of silence inserted for the
// gap, so the played-back timing matches what actually happened on air.
//
// Unlike HyteraTransceiver, this never touches the sound card at all --
// no mic, no speakers. It stores and replays the raw G.711 u-law bytes
// exactly as they came off the wire, with no PCM decode/re-encode round
// trip. That's both simpler and a more honest test of what the repeater
// itself did to the audio.
//
// NOTE: this is also a good way to finally settle an open question from
// this project -- RX parses incoming packets with a 29-byte header,
// while our own outgoing TX header is 28 bytes, and that 1-byte gap was
// never fully confirmed as intentional. If the echoed audio sounds
// clean, that split is fine as-is; if it sounds subtly shifted/garbled,
// that mismatch is the prime suspect.
// ==========================================================

#define LOCAL_IP             "192.168.1.136" // Your PC IP
#define PORT_RCP              30009          // Radio Control Port (TX only)
#define PORT_RTP              30012          // Voice/RTP Port (shared RX + TX)
#define TARGET_TALKGROUP      1
#define CALL_TYPE_GROUP       1

#define PAYLOAD_OFFSET        28             // CORRECTED from 29 -- confirmed by direct pcap
                                              // measurement: real downlink packets are exactly
                                              // 508 bytes = 28(header) + 480(3x160 audio frames),
                                              // and playback packets are 188 = 28+160. With 29
                                              // neither divides evenly by 160. This also settles
                                              // the old "28 vs 29" question noted in project notes.
#define NETWORK_BUF_SZ        2048
#define FRAME_BYTES           160            // One 20ms G.711 u-law frame
#define SILENCE_BYTE          0xFF           // Confirmed correct u-law silence encoding

#define SILENCE_TIMEOUT_MS    5000           // How long the channel must be truly quiet before we play back
#define MAX_RECORD_SECONDS    300            // 5 minute cap per capture
#define MAX_FRAMES            (MAX_RECORD_SECONDS * 50) // 50 frames/sec at 20ms each

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

#pragma pack(push, 1)
typedef struct {
    uint16_t fixed_marker;
    uint16_t seq_num;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t  hytera_pad[16];
    uint8_t  voice_payload[FRAME_BYTES];
} rtp_packet_t;
#pragma pack(pop)

typedef struct {
    SOCKET socket;
    struct sockaddr_in target_addr;
} keepalive_ctx_t;

static uint8_t  rcp_sequence_counter = 0;
static uint16_t rtp_sequence_counter = 0;
static uint32_t rtp_timestamp_counter = 0;

// The capture buffer -- raw u-law bytes, one 160-byte frame per slot.
static uint8_t frame_buffer[MAX_FRAMES][FRAME_BYTES];
static int frame_count = 0;

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

VOID CALLBACK SendKeepaliveCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer) {
    keepalive_ctx_t *ctx = (keepalive_ctx_t*)Context;
    sendto(ctx->socket, (const char*)KEEP_ALIVE_PAYLOAD, sizeof(KEEP_ALIVE_PAYLOAD), 0,
           (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

// Streams the captured frames back through the repeater at the correct
// 20ms cadence -- same Call Setup / PTT pattern as HyteraTransceiver's TX
// side, just sourcing frames from our buffer instead of a live mic.
void play_back_capture(SOCKET rcp_sock, SOCKET rtp_sock, struct sockaddr_in* remote_rcp_addr, struct sockaddr_in* remote_rtp_addr) {
    printf("[PLAYBACK] Silence confirmed -- playing back %d frames (%.1f seconds) captured audio...\n",
           frame_count, frame_count * 0.02f);

    printf("[RCP] Sending Call Setup for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);

    printf("[RCP] PTT Key-Up (playback)...\n");
    send_ptt_command(rcp_sock, remote_rcp_addr, 1);
    Sleep(100);

    for (int i = 0; i < frame_count; i++) {
        rtp_packet_t audio_pkt;
        memset(&audio_pkt, 0, sizeof(audio_pkt));

        audio_pkt.fixed_marker = htons(0x9000);
        audio_pkt.seq_num      = htons(rtp_sequence_counter++);
        audio_pkt.timestamp    = htonl(rtp_timestamp_counter);
        audio_pkt.ssrc         = 0;
        audio_pkt.hytera_pad[1] = 0x15;
        audio_pkt.hytera_pad[3] = 0x03;

        memcpy(audio_pkt.voice_payload, frame_buffer[i], FRAME_BYTES);

        sendto(rtp_sock, (const char*)&audio_pkt, sizeof(audio_pkt), 0,
               (struct sockaddr*)remote_rtp_addr, sizeof(*remote_rtp_addr));

        rtp_timestamp_counter += FRAME_BYTES;
        Sleep(20);
    }

    printf("[RCP] PTT De-Key (playback finished)...\n");
    send_ptt_command(rcp_sock, remote_rcp_addr, 0);
    printf("[SYSTEM] Playback complete. Listening for the next transmission...\n\n");
}

int main(int argc, char *argv[]) {
    // Without this, Windows' default timer granularity (~15.6ms) makes
    // Sleep(20) imprecise/jittery -- confirmed by comparison against a
    // real pcap capture that our own playback timing was the remaining
    // source of fragmentation after the frame-bundling fix. Same fix
    // HyteraTransceiver already uses for its own Sleep(20)-paced TX.
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
        (rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        WSACleanup();
        timeEndPeriod(1);
        return -1;
    }

    // Short receive timeout so the main loop can periodically check the
    // 5-second silence timeout even when no packets are arriving at all.
    DWORD rx_timeout = 100;
    setsockopt(rtp_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rx_timeout, sizeof(rx_timeout));

    // Larger receive buffer than the Windows default -- if this loop is
    // ever even briefly delayed (console I/O, OS scheduling), a bigger
    // buffer gives incoming packets somewhere to queue instead of the OS
    // silently discarding them. Cheap, safe, standard practice for a
    // steady real-time UDP stream.
    int rcvbuf_size = 262144; // 256KB
    setsockopt(rtp_sock, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf_size, sizeof(rcvbuf_size));

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
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup();
        timeEndPeriod(1);
        return -1;
    }

    printf("[AUTHOR] HyteraParrot made by Rob Thompson 2E0RPT...\n");

    // ------------------------------------------------------------
    // Resolve the repeater's IP: argv[1], or wait for it to speak first --
    // same pattern as HyteraTransceiver.
    // ------------------------------------------------------------
    char repeater_ip_str[INET_ADDRSTRLEN];
    if (argc >= 2) {
        struct in_addr testAddr;
        if (inet_pton(AF_INET, argv[1], &testAddr) != 1) {
            printf("[ERROR] '%s' is not a valid IPv4 address.\n", argv[1]);
            printf("        Usage: %s [repeater_ip]\n", argv[0]);
            closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup();
            timeEndPeriod(1);
            return -1;
        }
        strncpy(repeater_ip_str, argv[1], sizeof(repeater_ip_str) - 1);
        repeater_ip_str[sizeof(repeater_ip_str) - 1] = '\0';
        printf("[SYSTEM] Using repeater IP from command line: %s\n", repeater_ip_str);
    } else {
        printf("[SYSTEM] No repeater IP given -- waiting for the repeater to contact us on port %d...\n", PORT_RTP);
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
            waited_ms += 100;
            if (waited_ms % 5000 == 0) {
                printf("[SYSTEM] Still waiting for the repeater to speak first (%d s)...\n", waited_ms / 1000);
            }
        }
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
        FILETIME ftDueTime; ULARGE_INTEGER ulDueTime; ulDueTime.QuadPart = 0;
        ftDueTime.dwHighDateTime = ulDueTime.HighPart; ftDueTime.dwLowDateTime = ulDueTime.LowPart;
        SetThreadpoolTimer(rcp_timer, &ftDueTime, 5000, 0);
        SetThreadpoolTimer(rtp_timer, &ftDueTime, 5000, 0);
        printf("[SYSTEM] Asynchronous 5-second keepalives armed.\n");
    }

    printf("[SYSTEM] Holding link stabilization window for 4 seconds...\n");
    Sleep(4000);

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    printf("[SYSTEM] Ready. Listening for a station to transmit. Press ESC to quit.\n\n");

    // ------------------------------------------------------------
    // Main capture/playback loop
    // ------------------------------------------------------------
    int recording = 0;
    int max_length_warned = 0;
    DWORD last_packet_time = 0;
    DWORD first_packet_time = 0;
    uint32_t current_radio_id = 0;

    char net_buffer[NETWORK_BUF_SZ];
    struct sockaddr_in client_addr;
    int client_addr_len;

    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            printf("[SYSTEM] Exiting.\n");
            break;
        }

        client_addr_len = sizeof(client_addr);
        int bytes_received = recvfrom(rtp_sock, net_buffer, NETWORK_BUF_SZ, 0,
                                      (struct sockaddr *)&client_addr, &client_addr_len);
        DWORD now = GetTickCount();

        if (bytes_received > PAYLOAD_OFFSET) {
            // A real audio-bearing packet arrived.
            uint8_t *ubuf = (uint8_t *)net_buffer;
            uint32_t parsed_id = 0;
            if (bytes_received >= 23) {
                parsed_id = ((uint32_t)ubuf[17] << 16) | ((uint32_t)ubuf[18] << 8) | (uint32_t)ubuf[19];
            }

            int payload_len = bytes_received - PAYLOAD_OFFSET;

            if (!recording) {
                printf("[CAPTURE] Station detected (Radio ID %u) -- recording...\n", parsed_id);
                frame_count = 0;
                recording = 1;
                current_radio_id = parsed_id;
                first_packet_time = now;
                max_length_warned = 0;
            } else {
                // Back-fill silence for the gap since the last packet --
                // this is what makes a momentary fade-out/fade-back-in
                // sound correct on playback instead of clipped together.
                DWORD gap_ms = now - last_packet_time;
                // 300ms, not 40ms: real-world testing showed gaps of up to
                // ~100ms happening on nearly every frame transition even
                // during unbroken speech -- ordinary network/OS jitter, not
                // a real dropout. Treating those as gaps and backfilling
                // silence for them chopped continuous speech into audible
                // stutters. 300ms comfortably clears that noise floor while
                // still catching genuinely perceptible fades. (Still a safe
                // margin now that packets can arrive every ~60ms instead of
                // every 20ms, since the repeater bundles 3 frames/packet.)
                if (gap_ms > 300) {
                    int silence_frames = gap_ms / 20;
                    for (int s = 0; s < silence_frames && frame_count < MAX_FRAMES; s++) {
                        memset(frame_buffer[frame_count], SILENCE_BYTE, FRAME_BYTES);
                        frame_count++;
                    }
                    printf("[CAPTURE] Signal dropped out for %.1fs and came back -- inserted matching silence.\n", gap_ms / 1000.0f);
                }
            }

            // The repeater bundles multiple 20ms frames into one UDP packet
            // (confirmed via pcap: downlink packets are 480 bytes of audio =
            // 3 frames, not 1) -- extract every full frame present, not just
            // the first. This was the actual bug behind the "stuttery/faster"
            // playback: 2 out of every 3 frames were being silently dropped.
            int num_frames_in_packet = payload_len / FRAME_BYTES;
            int remainder_bytes = payload_len % FRAME_BYTES;

            for (int fnum = 0; fnum < num_frames_in_packet && frame_count < MAX_FRAMES; fnum++) {
                memcpy(frame_buffer[frame_count], ubuf + PAYLOAD_OFFSET + (fnum * FRAME_BYTES), FRAME_BYTES);
                frame_count++;
            }
            if (remainder_bytes > 0 && frame_count < MAX_FRAMES) {
                // Trailing partial frame (e.g. the last frame of a
                // transmission cut slightly short) -- pad with silence.
                uint8_t *frame = frame_buffer[frame_count];
                memcpy(frame, ubuf + PAYLOAD_OFFSET + (num_frames_in_packet * FRAME_BYTES), remainder_bytes);
                memset(frame + remainder_bytes, SILENCE_BYTE, FRAME_BYTES - remainder_bytes);
                frame_count++;
            }
            if (frame_count >= MAX_FRAMES && !max_length_warned) {
                printf("[WARNING] Max capture length (%d seconds) reached -- further audio in this transmission will be dropped, but timing/playback will still proceed normally.\n", MAX_RECORD_SECONDS);
                max_length_warned = 1;
            }

            last_packet_time = now;
        }

        if (recording && (now - last_packet_time) >= SILENCE_TIMEOUT_MS) {
            recording = 0;
            if (frame_count > 0) {
                // Diagnostic: compare how many frames we'd expect for the
                // real elapsed time (first packet to last packet, at one
                // frame per 20ms) against how many we actually captured.
                // A large gap between these numbers confirms real packet
                // loss during capture, rather than a timing/logic bug.
                DWORD real_span_ms = last_packet_time - first_packet_time;
                int expected_frames = (real_span_ms / 20) + 1;
                if (expected_frames > 0) {
                    float loss_pct = 100.0f * (1.0f - ((float)frame_count / (float)expected_frames));
                    printf("[DIAGNOSTIC] Real transmission spanned %lums -- expected ~%d frames, captured %d (%.0f%% apparent loss).\n",
                           real_span_ms, expected_frames, frame_count, loss_pct);
                }
                play_back_capture(rcp_sock, rtp_sock, &remote_rcp_addr, &remote_rtp_addr);
            }
            frame_count = 0;
        }
    }

    if (rcp_timer) { SetThreadpoolTimer(rcp_timer, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(rcp_timer, TRUE); CloseThreadpoolTimer(rcp_timer); }
    if (rtp_timer)  { SetThreadpoolTimer(rtp_timer, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(rtp_timer, TRUE); CloseThreadpoolTimer(rtp_timer); }

    closesocket(rcp_sock);
    closesocket(rtp_sock);
    WSACleanup();
    timeEndPeriod(1);
    printf("[DONE] HyteraParrot session completed cleanly.\n");
    return 0;
}
