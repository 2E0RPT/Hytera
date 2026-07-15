#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> 
#include <time.h>   

// --- Windows-Specific Network and System Headers ---
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h> 
#pragma comment(lib, "ws2_32.lib") 
#pragma comment(lib, "winmm.lib")  

#define close closesocket

// --- Base Configuration Constants ---
#define LOCAL_IP             "192.168.1.136" 
#define REPEATER_IP          "192.168.1.167" 
#define PORT_RCP             30009           // Slot 1 Control Port
#define PORT_RTP             30012           // Slot 1 Audio Port
#define AUDIO_FILE_NAME      "input.raw"     

#define DEFAULT_TALKGROUP    1               // Baseline fallback target group
#define CALL_TYPE_GROUP      1               

// --- Pre-Verified, Hardcoded Structural Memory Templates ---
const uint8_t WAKE_STATIC_RAW[]      = {0x32, 0x42, 0x00, 0x05, 0x00, 0x00};
const uint8_t KEEPALIVE_STATIC_RAW[] = {0x32, 0x42, 0x00, 0x02, 0x00, 0x00};

// Exact 18-Byte Call Setup Layout for Talkgroup 1 (From your functional Wireshark capture Frame 5)
const uint8_t CALL_SETUP_TG1[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00, 
    0x02, 0x41, 0x08, 0x05, 0x00, 0x01, 
    0x01, 0x00, 0x00, 0x00, 0x5E, 0x03  
};

// Exact 18-Byte Call Setup Layout specifically formatted for Talkgroup 2
const uint8_t CALL_SETUP_TG2[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00, 
    0x02, 0x41, 0x08, 0x05, 0x00, 0x01, 
    0x02, 0x00, 0x00, 0x00, 0x5E, 0x03  
};

// Exact 18-Byte Call Setup Layout specifically formatted for Emergency Talkgroup 99
const uint8_t CALL_SETUP_TG99[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00, 
    0x02, 0x41, 0x08, 0x05, 0x00, 0x01, 
    0x63, 0x00, 0x00, 0x00, 0x5E, 0x03  
};

// Base command layout structure
const uint8_t PTT_TEMPLATE_BASE[] = {
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
    uint8_t  hytera_pad[16]; // FIXED: Allocated explicit 16-byte array footprint bounds
    uint8_t  voice_payload[160]; // FIXED: Allocated explicit 160-byte real data container
} rtp_packet_t;
#pragma pack(pop)

static uint8_t rcp_sequence_counter = 0;
static uint16_t rtp_sequence_counter = 0;
static uint32_t rtp_timestamp_counter = 0;

uint8_t get_next_rcp_seq(void) {
    return rcp_sequence_counter++;
}

// Aligned PTT transmission function using explicit tracking counter assignments
void send_ptt_command_packet(SOCKET sock, struct sockaddr_in* addr, int turn_on, uint8_t sequence_id) {
    uint8_t packet[sizeof(PTT_TEMPLATE_BASE)];
    memcpy(packet, PTT_TEMPLATE_BASE, sizeof(PTT_TEMPLATE_BASE));
    
    // Forces the sequence counter to lock cleanly to the target session ID index
    packet[5] = sequence_id; 
    
    if (turn_on) {
        packet[12] = 0x01;
        packet[13] = 0xEB;
    } else {
        packet[12] = 0x00;
        packet[13] = 0xEC;
    }
    
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

VOID CALLBACK SendKeepaliveCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer) {
    keepalive_ctx_t *ctx = (keepalive_ctx_t*)Context;
    sendto(ctx->socket, (const char*)KEEPALIVE_STATIC_RAW, sizeof(KEEPALIVE_STATIC_RAW), 0, 
           (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

int main(int argc, char *argv[]) {
    int selected_tg = 1; 
    const uint8_t *setup_template_ptr = NULL;

    if (argc > 1) {
        selected_tg = atoi(argv[1]);
    }

    if (selected_tg == 2) {
        setup_template_ptr = CALL_SETUP_TG2;
    } else if (selected_tg == 99) {
        setup_template_ptr = CALL_SETUP_TG99;
    } else {
        selected_tg = 1; 
        setup_template_ptr = CALL_SETUP_TG1;
    }

    FILE *audio_file = fopen(AUDIO_FILE_NAME, "rb");
    if (audio_file == NULL) {
        printf("[ERROR] Cannot open audio file '%s'.\n", AUDIO_FILE_NAME);
        return -1;
    }

    timeBeginPeriod(1);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock initialization failed.\n");
        fclose(audio_file);
        timeEndPeriod(1);
        return -1;
    }

    SOCKET rcp_sock, rtp_sock;
    struct sockaddr_in local_rcp_addr, local_rtp_addr;
    struct sockaddr_in remote_rcp_addr, remote_rtp_addr;

    if ((rcp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET || 
        (rtp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        fclose(audio_file); WSACleanup(); timeEndPeriod(1);
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
        close(rcp_sock); close(rtp_sock); if (audio_file) fclose(audio_file); WSACleanup(); timeEndPeriod(1);
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
    sendto(rcp_sock, (const char*)WAKE_STATIC_RAW, sizeof(WAKE_STATIC_RAW), 0, (struct sockaddr*)&remote_rcp_addr, sizeof(remote_rcp_addr));
    sendto(rtp_sock, (const char*)WAKE_STATIC_RAW, sizeof(WAKE_STATIC_RAW), 0, (struct sockaddr*)&remote_rtp_addr, sizeof(remote_rtp_addr));

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
    uint8_t current_session_seq = get_next_rcp_seq(); // Allocates Sequence 00 00 safely
    
    // FIXED: Allocated secure 18-byte container block layout to prevent pointer overflow mutations
    uint8_t active_setup_packet[18];
    memcpy(active_setup_packet, setup_template_ptr, 18);
    active_setup_packet[5] = current_session_seq; // Injects Sequence 00 00 matching working trace perfectly

    printf("[RCP] Sending Aligned Call Setup Envelope targeting Talkgroup %d...\n", selected_tg);
    sendto(rcp_sock, (const char*)active_setup_packet, 18, 0, (struct sockaddr*)&remote_rcp_addr, sizeof(remote_rcp_addr));
    
    // 10ms pacing gap matching your functional trace perfectly
    Sleep(10); 

    printf("[RCP] Broadcasting PTT Key-Up Command...\n");
    // Increments sequence index safely to match working Frame 7 layout sequence
    send_ptt_command_packet(rcp_sock, &remote_rcp_addr, 1, current_session_seq + 1); 
    
    printf("[SYSTEM] Holding 2-second amplifier pre-roll stabilization pause...\n");
    Sleep(2000); 

    // ========================================================================
    // PHASE 2: AUDIO STREAMING (100% UNTOUCHED NATIVE VERSION 6.0 AUDIO CORE)
    // ========================================================================
    printf("[RTP] Streaming audio from '%s' over Slot 1 to TG %d...\n", AUDIO_FILE_NAME, selected_tg);
    
    uint8_t read_buffer[160]; 
    size_t bytes_read = 0;

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    while ((bytes_read = fread(read_buffer, 1, 160, audio_file)) > 0) {
        rtp_packet_t audio_pkt;
        memset(&audio_pkt, 0, sizeof(audio_pkt));

        audio_pkt.fixed_marker = htons(0x9000);
        audio_pkt.seq_num      = htons(rtp_sequence_counter++);
        audio_pkt.timestamp    = htonl(rtp_timestamp_counter);
        audio_pkt.ssrc         = htonl(0x11223344); 

        audio_pkt.hytera_pad[0] = 0x15;
        audio_pkt.hytera_pad[1] = 0x03;

        memcpy(audio_pkt.voice_payload, read_buffer, bytes_read);

        if (bytes_read < 160) {
            memset(audio_pkt.voice_payload + bytes_read, 0xFF, 160 - bytes_read);
        }

        sendto(rtp_sock, (const char*)&audio_pkt, sizeof(audio_pkt), 0, (struct sockaddr*)&remote_rtp_addr, sizeof(remote_rtp_addr));
        rtp_timestamp_counter += 160;
		Sleep(20);
	}
	printf("[RTP] Audio source stream completed successfully.\n");
	// ==========================================
	// PHASE 3: TRANSMIT PTT DE-KEY & CLEANUP
	// ==========================================
	printf("[RCP] Broadcasting PTT De-Key Command...\n");
	// Closes session sequentially matching strict counter logic safely
	send_ptt_command_packet(rcp_sock, &remote_rcp_addr, 0, current_session_seq + 2);
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
	
	fclose(audio_file);
	close(rcp_sock);
	close(rtp_sock);
	WSACleanup();
	timeEndPeriod(1);
	printf("[DONE] Session completed cleanly.\n");
	return 0;
}
