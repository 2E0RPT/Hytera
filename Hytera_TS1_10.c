#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Enable Winsock2 specifications for Windows Vista/7/10+
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <math.h>

#define UDP_PORT        30012      // Match your Hytera IP Multi-Site service port
#define BUFFER_COUNT    4          // Number of ring buffers for audio queuing
#define PAYLOAD_OFFSET  29         // Adjust based on your Hytera header/RTP length
#define NETWORK_BUF_SZ  2048       // Max expected UDP packet size
#define VU_METER_WIDTH  60         // Maximum character width of the ASCII VU bar

// Global audio structure
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHeaders[BUFFER_COUNT];
int currentBufferIndex = 0;

// Structure to pass network parameters safely to the background heartbeat thread
typedef struct {
    SOCKET socket_fd;
    struct sockaddr_in target_addr;
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

    char *ip_str = inet_ntoa(params->target_addr.sin_addr);
    unsigned short port = ntohs(params->target_addr.sin_port);
    
    printf("[Heartbeat Thread] First packet received. Sending responses back to repeater at: %s:%d\n", ip_str, port);

    while (1) {
        // Wait for 5 seconds (5000 milliseconds)
        Sleep(5000);

        int bytes_sent = sendto(params->socket_fd, (char *)heartbeat_packet, packet_len, 0,
                                (struct sockaddr *)&params->target_addr, sizeof(params->target_addr));

        if (bytes_sent == SOCKET_ERROR) {
            fprintf(stderr, "\n[Heartbeat Thread] Warning: Failed to send periodic packet. Error: %d\n", WSAGetLastError());
        } else {
            // Suppressing generic spam print to protect the ASCII lines from vertical displacement
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
    
    // Track the currently speaking radio ID to check for transitions
    uint32_t current_radio_id = 0;
    int is_first_radio_id_parsed = 0;

    // VU meter and Timing State metrics
    int audio_packet_counter = 0;
    double accumulated_amplitude_sum = 0.0;
    long long total_samples_accumulated = 0;
    DWORD last_audio_packet_time = 0;
    int is_currently_receiving_stream = 0;

    // Live Hotkey Volume State Metrics (Starts at 100%, adjustments occur in 50% steps)
    float volume_multiplier = 1.0f;

    init_ulaw_lut();
    init_audio_device(8000); // G.711 standard telemetry sample rate

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "Winsock initialization failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    // Set up standard UDP Listening Socket with non-blocking configurations to check 2-second limits
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Set a short receive timeout (e.g., 100ms) so our loop can frequently verify our 2-second timeout window
    DWORD timeout = 100;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(UDP_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Socket binding failed. Error: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    printf("HyteraTS1 v1.0 by Rob Thompson 2E0RPT.\n");
	printf("Monitor live voice on time slot 1.\n");
	printf("Use the Up & Down kays to adjust the volume.\n");
    printf("Listening for Hytera repeater UDP stream on port %d...\n", UDP_PORT);
	fflush(stdout);

    // Main real-time process processing loop
    while (1) {
        // --- Live Keyboard Hotkey Scan Section ---
        // Checks if the window has focus before processing hotkeys to prevent background system disruption
        if (GetForegroundWindow() == GetConsoleWindow() && is_heartbeat_started == 1) {
            if (GetAsyncKeyState(VK_UP) & 0x0001) {
                volume_multiplier += 0.5f;
                if (volume_multiplier > 5.0f) volume_multiplier = 5.0f;
                printf("\r[Volume Set]: %3.0f%%                                                      \n", volume_multiplier * 100.0f);
                fflush(stdout);
            }
            if (GetAsyncKeyState(VK_DOWN) & 0x0001) {
                volume_multiplier -= 0.5f;
                if (volume_multiplier < 0.0f) volume_multiplier = 0.0f;
                printf("\r[Volume Set]: %3.0f%%                                                      \n", volume_multiplier * 100.0f);
                fflush(stdout);
            }
        }

        client_addr_len = sizeof(client_addr);
        int bytes_received = recvfrom(server_fd, net_buffer, NETWORK_BUF_SZ, 0, 
                                      (struct sockaddr *)&client_addr, &client_addr_len);
        
        DWORD current_time = GetTickCount();

        // 2-second carrier dropped silence tracking logic
        if (is_currently_receiving_stream && (current_time - last_audio_packet_time >= 2000)) {
            // Push out an empty clear frame to blank the trailing VU bar elements cleanly
            printf("\rAudio Stream Idle.                                                                       \n");
            is_currently_receiving_stream = 0;
            is_first_radio_id_parsed = 0; // Prepare to print transmission details cleanly on next stream load
        }

        if (bytes_received <= 0) {
            // Socket timeout or error occurred - jump back up to prevent decoding empty fields
            continue; 
        }

        // Start heartbeat thread immediately when the first packet hits the port
        if (!is_heartbeat_started) {
            printf("Connected.\n");

            HeartbeatParams *params = (HeartbeatParams *)malloc(sizeof(HeartbeatParams));
            if (params != NULL) {
                params->socket_fd = server_fd;
                params->target_addr = client_addr; 

                HANDLE hThread = CreateThread(NULL, 0, HeartbeatThreadFunc, params, 0, NULL);
                if (hThread != NULL) {
                    CloseHandle(hThread); 
                    is_heartbeat_started = 1;
                } else {
                    fprintf(stderr, "Error: Failed to spin up heartbeat thread.\n");
                    free(params);
                }
            }
        }

        // Process Radio ID and Group Target fields if the packet contains the necessary headers
        if (bytes_received >= 23) {
            uint8_t *ubuf = (uint8_t *)net_buffer;
            
            // Extract 3 bytes from indices 17, 18, 19 as a 24-bit big-endian value (Radio ID)
            uint32_t parsed_id = ((uint32_t)ubuf[17] << 16) | 
                                 ((uint32_t)ubuf[18] << 8)  | 
                                  (uint32_t)ubuf[19];

            // Extract 3 bytes from indices 20, 21, 22 as a 24-bit big-endian value (Target/Group ID)
            uint32_t parsed_group = ((uint32_t)ubuf[20] << 16) | 
                                   ((uint32_t)ubuf[21] << 8)  | 
                                    (uint32_t)ubuf[22];

            // Print on the screen only once, and update only when the Radio ID changes
            if (!is_first_radio_id_parsed || parsed_id != current_radio_id) {
                current_radio_id = parsed_id;
                is_first_radio_id_parsed = 1;
                
                // If we were already drawing a VU meter, append a newline to split the old stream log safely
                if (is_currently_receiving_stream) {
                    printf("\n");
                }
                printf("Radio ID: %u talking to Group %u\n", current_radio_id, parsed_group);
                is_currently_receiving_stream = 1;
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
            int16_t sample = ulaw_to_pcm_lut[ulaw_ptr[i]];
            
            // Apply hotkey volume multiplier
            float amplified_sample = (float)sample * volume_multiplier;
            
            // Safe clamping bounds configuration to handle excessive volume scaling
            if (amplified_sample > 32767.0f) {
                sample = 32767;
            } else if (amplified_sample < -32768.0f) {
                sample = -32768;
            } else {
                sample = (int16_t)amplified_sample;
            }

            pcm_out[i] = sample;
			
            // Calculate Absolute Amplitude for Average VU computation calculations
            accumulated_amplitude_sum += abs(sample);
			total_samples_accumulated++;
			}

            // Keep timestamp updated as long as audio packets flow down the socket pipeline
			last_audio_packet_time = GetTickCount();
			is_currently_receiving_stream = 1;
			audio_packet_counter++;
			
			// Render ASCII VU meter calculations once every 10 valid audio packets
			if (audio_packet_counter >= 3) {
				double average_amplitude = 0.0;
				if (total_samples_accumulated > 0) {
					average_amplitude = accumulated_amplitude_sum / total_samples_accumulated;
				}
				
				// Normalise the average amplitude (0 to 32768 max depth) down into our bar resolution layout
				// Normal human voice peaks scale nicely down a logarithmic or linear ceiling of ~8000 for G.711 telephony
				int bar_elements_to_fill = (int)((average_amplitude / 8000.0) * VU_METER_WIDTH);
				if (bar_elements_to_fill > VU_METER_WIDTH) bar_elements_to_fill = VU_METER_WIDTH;
				if (bar_elements_to_fill < 0) bar_elements_to_fill = 0;
				
				// Render Horizontal Single-Line interface frame using carriage return '\r'
				printf("\r[");
				for (int b = 0; b < VU_METER_WIDTH; b++) {
					if (b < bar_elements_to_fill) {
						if (b > (int)(VU_METER_WIDTH * 0.8)) {
							printf("#"); // Peak indicator range (Over-driven signal levels)
							} else if (b > (int)(VU_METER_WIDTH * 0.5)) {
								printf("="); // Mid signal range
						    } else {printf("-"); // Normal baseline stream range
					    }
				    } else {
				        printf(" "); // Empty dynamic filler block space
				    }
			    }
		// Displays both audio signal intensity and the real-time volume gain scalar percentage
        printf("] Level: %4.0f | Gain: %3.0f%%", average_amplitude, volume_multiplier * 100.0f);
		fflush(stdout); // Flush standard display registers to ensure fast presentation adjustments
		// Clear aggregation counters for the next block interval loop
		audio_packet_counter = 0;
		accumulated_amplitude_sum = 0.0;
		total_samples_accumulated = 0;
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
	WSACleanup();for (int i = 0; i < BUFFER_COUNT; i++) {
	waveOutUnprepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
	free(waveHeaders[i].lpData);
	}
waveOutClose(hWaveOut);
return 0;
}