#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Targets Windows Vista / 7 / 10 and above to enable modern Winsock APIs
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>

#define UDP_PORT        30012      // Match your Hytera IP Multi-Site service port
#define BUFFER_COUNT    4          // Number of ring buffers for audio queuing
#define PAYLOAD_OFFSET  29         // Adjust based on your Hytera header/RTP length
#define NETWORK_BUF_SZ  2048       // Max expected UDP packet size

// Global audio structure
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHeaders[BUFFER_COUNT];
int currentBufferIndex = 0;

// Structure to pass network parameters safely to the background heartbeat thread
typedef struct {
    SOCKET socket_fd;
    struct sockaddr_in target_addr; // Dynamically stores the sender's IP/Port
} HeartbeatParams;

// G.711 u-Law to 16-bit Linear PCM Lookup Table (LUT)
static int16_t ulaw_to_pcm_lut[256];

void init_ulaw_lut(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t data = ~i; // Invert to process sign-magnitude
        int sign = (data & 0x80);
        int exponent = (data >> 4) & 0x07;
        int mantissa = data & 0x0F;
        int sample = ((mantissa << 3) + 33) << exponent;
        sample -= 33;
        ulaw_to_pcm_lut[i] = (int16_t)(sign ? -sample : sample);
    }
}

// Visual Studio / GCC cross-compatibility for standard Windows WaveOut Audio Engine
void init_audio_device(int sample_rate) {
    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;                     // Mono stream from repeater
    wfx.nSamplesPerSec = sample_rate;       // Typically 8000Hz for G.711
    wfx.wBitsPerSample = 16;                // Decoded target depth
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "Error: Failed to open Windows WAVE_MAPPER device.\n");
        exit(1);
    }

    // Allocate and prepare cyclic playing audio buffers
    for (int i = 0; i < BUFFER_COUNT; i++) {
        waveHeaders[i].dwBufferLength = NETWORK_BUF_SZ * 2; // Decoded size is double 
        waveHeaders[i].lpData = (char *)malloc(waveHeaders[i].dwBufferLength);
        waveHeaders[i].dwFlags = 0;
        waveOutPrepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
        waveHeaders[i].dwFlags |= WHDR_DONE; // Mark initially empty
    }
}

// Background thread function that handles the 5-second interval heartbeat
DWORD WINAPI HeartbeatThreadFunc(LPVOID lpParam) {
    HeartbeatParams *params = (HeartbeatParams *)lpParam;
    
    // Explicit 6-byte payload specified: 32 42 00 05 00 00
    unsigned char heartbeat_packet[] = {0x32, 0x42, 0x00, 0x05, 0x00, 0x00};
    int packet_len = sizeof(heartbeat_packet);

    // Extract the sender's IP address string cleanly using inet_ntoa
    char *ip_str = inet_ntoa(params->target_addr.sin_addr);
    unsigned short port = ntohs(params->target_addr.sin_port);
    
    printf("[Heartbeat Thread] Sending responses back to repeater at: %s:%d\n", ip_str, port);

    while (1) {
        // Wait for 5 seconds (5000 milliseconds)
        Sleep(5000);

        // This sends the packet to the exact address stored from the first incoming packet
        int bytes_sent = sendto(params->socket_fd, (char *)heartbeat_packet, packet_len, 0,
                                (struct sockaddr *)&params->target_addr, sizeof(params->target_addr));

        if (bytes_sent == SOCKET_ERROR) {
            fprintf(stderr, "[Heartbeat Thread] Warning: Failed to send periodic packet. Error: %d\n", WSAGetLastError());
        } else {
            printf("[Heartbeat Thread] Periodic packet sent back to repeater.\n");
        }
    }

    free(params);
    return 0;
}

int main(void) {
    WSADATA wsaData;
    SOCKET server_fd;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_len = sizeof(client_addr);
    char net_buffer[NETWORK_BUF_SZ];
    
    int is_heartbeat_started = 0;

    init_ulaw_lut();
    init_audio_device(8000); // G.711 standard telemetry sample rate

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "Winsock initialization failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    // Set up standard UDP Listening Socket
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(UDP_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Socket binding failed. Error: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    printf("Listening for Hytera RD985 UDP stream on port %d...\n", UDP_PORT);

    // Main real-time process processing loop
    while (1) {
        // Reset client address length structure for every incoming packet
        client_addr_len = sizeof(client_addr);
        
        int bytes_received = recvfrom(server_fd, net_buffer, NETWORK_BUF_SZ, 0, 
                                      (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (bytes_received < 6) continue;

        // Check if the heartbeat thread hasn't started yet AND if the packet matches the trigger signature
        if (!is_heartbeat_started) {
            uint8_t *ubuf = (uint8_t *)net_buffer;
            
            // FIXED: Using array syntax indexes [0]-[5] so GCC checks the buffer bytes correctly
            if (ubuf[0] == 0x32 && ubuf[1] == 0x42 && ubuf[2] == 0x00 && 
                ubuf[3] == 0x02 && ubuf[4] == 0x00 && ubuf[5] == 0x00) {
                
                printf("Connected.\n");

                HeartbeatParams *params = (HeartbeatParams *)malloc(sizeof(HeartbeatParams));
                if (params != NULL) {
                    params->socket_fd = server_fd;
                    // Securely duplicate the whole client address structure including target port and dynamic IP
                    params->target_addr = client_addr; 

                    // Spawn the concurrent thread to manage the 5-second interval loop
                    HANDLE hThread = CreateThread(NULL, 0, HeartbeatThreadFunc, params, 0, NULL);
                    if (hThread != NULL) {
                        CloseHandle(hThread); // Detach handle since we don't need to join it explicitly
                        is_heartbeat_started = 1;
                    } else {
                        fprintf(stderr, "Error: Failed to spin up heartbeat thread.\n");
                        free(params);
                    }
                }
            }
        }

        // Safety barrier: Skip parsing if packet is strictly header telemetry data without audio payload
        if (bytes_received <= PAYLOAD_OFFSET) continue;

        int audio_payload_len = bytes_received - PAYLOAD_OFFSET;
        uint8_t *ulaw_ptr = (uint8_t *)(net_buffer + PAYLOAD_OFFSET);

        // Find an available buffer in the audio pipeline
        WAVEHDR *hdr = &waveHeaders[currentBufferIndex];
        
        // Simple spin-lock to safely handle incoming network traffic spikes
        while (!(hdr->dwFlags & WHDR_DONE)) {
            Sleep(1); 
        }

        // Decode 8-bit u-law directly into the Windows 16-bit PCM audio buffer
        int16_t *pcm_out = (int16_t *)hdr->lpData;
        for (int i = 0; i < audio_payload_len; i++) {
            pcm_out[i] = ulaw_to_pcm_lut[ulaw_ptr[i]];
        }

        // Submit block directly to the audio driver hardware layer
        hdr->dwBufferLength = audio_payload_len * sizeof(int16_t);
        hdr->dwFlags &= ~WHDR_DONE;
        waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));

        // Increment pointer index to use the next ring buffer segment
        currentBufferIndex = (currentBufferIndex + 1) % BUFFER_COUNT;
    }

    // Resource Cleanup 
    closesocket(server_fd);
    WSACleanup();
    for (int i = 0; i < BUFFER_COUNT; i++) {
        waveOutUnprepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
        free(waveHeaders[i].lpData);
    }
    waveOutClose(hWaveOut);
    return 0;
}
