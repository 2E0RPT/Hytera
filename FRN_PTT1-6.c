#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmsystem.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

// --- Configuration Constants ---
#define MULTICAST_IP     "239.0.0.1"      
#define MULTICAST_PORT   10024           
#define SAMPLE_RATE      8000            
#define GSM_SAMPLES      160              
#define GSM_FRAME_SIZE   33               

#define FRAMES_PER_BLOCK 4                
#define BLOCK_SAMPLES    (GSM_SAMPLES * FRAMES_PER_BLOCK) 
#define BLOCK_PCM_SIZE   (BLOCK_SAMPLES * 2)             
#define BLOCK_NET_SIZE   (GSM_FRAME_SIZE * FRAMES_PER_BLOCK) 

#define MODE_CAPSLOCK    0
#define MODE_SPACEBAR    1

// --- libgsm Typedefs ---
typedef void* gsm;
typedef gsm   (*GsmCreateFunc)(void);
typedef void  (*GsmDestroyFunc)(gsm);
typedef int   (*GsmEncodeFunc)(gsm, short*, unsigned char*);
typedef int   (*GsmDecodeFunc)(gsm, unsigned char*, short*);

// --- Global Structural Handles ---
gsm encoder_state = NULL;
gsm decoder_state = NULL;
SOCKET net_socket = INVALID_SOCKET;
struct sockaddr_in group_addr;

GsmEncodeFunc gsm_encode_ptr = NULL;
GsmDecodeFunc gsm_decode_ptr = NULL;

// --- Global Audio Pipeline Variables ---
HWAVEIN hWaveInGlobal;
WAVEHDR inHdrs[3];                        
short mic_buffers[3][BLOCK_SAMPLES];
int current_in_buf = 0;
BOOL volatile bTransmitActive = FALSE;
int g_ptt_mode = MODE_CAPSLOCK; 

// --- Network Receiver Thread with Stream ID Logging ---
void receive_thread(void* param) {
    HWAVEOUT hWaveOut = (HWAVEOUT)param;
    unsigned char incoming_block[BLOCK_NET_SIZE];
    short decoded_block[BLOCK_SAMPLES];
    
    WAVEHDR waveHdr[2];
    short pcm_buffers[2][BLOCK_SAMPLES];
    int buf_idx = 0;
    
    BOOL bIsNewStream = TRUE;
    DWORD dwLastPacketTime = 0;

    for (int i = 0; i < 2; i++) {
        ZeroMemory(&waveHdr[i], sizeof(WAVEHDR));
        waveHdr[i].lpData = (LPSTR)pcm_buffers[i];
        waveHdr[i].dwBufferLength = BLOCK_PCM_SIZE;
    }

    struct sockaddr_in sender_addr;
    int addr_len = sizeof(sender_addr);

    while (1) {
        addr_len = sizeof(sender_addr);
        int bytes_rx = recvfrom(net_socket, (char*)incoming_block, BLOCK_NET_SIZE, 0, 
                                (struct sockaddr*)&sender_addr, &addr_len);
        
        if (bytes_rx == BLOCK_NET_SIZE && !bTransmitActive) {
            DWORD dwCurrentTime = GetTickCount();
            
            // If more than 800ms has elapsed since the last packet, treat it as a new stream
            if (bIsNewStream || (dwCurrentTime - dwLastPacketTime > 800)) {
                printf("\n[AUDIO] Receiving voice stream from sender IP: %s\n", inet_ntoa(sender_addr.sin_addr));
                bIsNewStream = FALSE;
            }
            dwLastPacketTime = dwCurrentTime;

            for (int f = 0; f < FRAMES_PER_BLOCK; f++) {
                gsm_decode_ptr(
                    decoder_state, 
                    &incoming_block[f * GSM_FRAME_SIZE], 
                    &decoded_block[f * GSM_SAMPLES]
                );
            }

            if (waveHdr[buf_idx].dwFlags & WHDR_PREPARED) {
                while (!(waveHdr[buf_idx].dwFlags & WHDR_DONE)) {
                    Sleep(1); 
                }
                waveOutUnprepareHeader(hWaveOut, &waveHdr[buf_idx], sizeof(WAVEHDR));
            }

            CopyMemory(pcm_buffers[buf_idx], decoded_block, BLOCK_PCM_SIZE);
            waveOutPrepareHeader(hWaveOut, &waveHdr[buf_idx], sizeof(WAVEHDR));
            waveOutWrite(hWaveOut, &waveHdr[buf_idx], sizeof(WAVEHDR));

            buf_idx = (buf_idx + 1) % 2; 
        } else {
            bIsNewStream = TRUE;
            Sleep(10);
        }
    }
}

// --- Dedicated Transmit Thread ---
void transmit_thread(void* param) {
    unsigned char outbound_block[BLOCK_NET_SIZE];
    (void)param;
    
    while (1) {
        if (bTransmitActive) {
            WAVEHDR* pHdr = &inHdrs[current_in_buf];
            
            while (!(pHdr->dwFlags & WHDR_DONE)) {
                Sleep(1);
                if (!bTransmitActive) break;
            }
            
            if (!bTransmitActive) continue;

            waveInUnprepareHeader(hWaveInGlobal, pHdr, sizeof(WAVEHDR));
            
            for (int f = 0; f < FRAMES_PER_BLOCK; f++) {
                gsm_encode_ptr(
                    encoder_state, 
                    &mic_buffers[current_in_buf][f * GSM_SAMPLES], 
                    &outbound_block[f * GSM_FRAME_SIZE]
                );
            }

            sendto(net_socket, (char*)outbound_block, BLOCK_NET_SIZE, 0, 
                   (struct sockaddr*)&group_addr, sizeof(group_addr));

            waveInPrepareHeader(hWaveInGlobal, pHdr, sizeof(WAVEHDR));
            waveInAddBuffer(hWaveInGlobal, pHdr, sizeof(WAVEHDR));

            current_in_buf = (current_in_buf + 1) % 3;
        } else {
            Sleep(10);
        }
    }
}

// --- Helper Function to Evaluate Chosen Hotkey Hardware State ---
BOOL is_ptt_pressed() {
    if (g_ptt_mode == MODE_SPACEBAR) {
        return (GetAsyncKeyState(VK_SPACE) & 0x8000) ? TRUE : FALSE;
    } else {
        return (GetKeyState(VK_CAPITAL) & 0x0001) ? TRUE : FALSE;
    }
}

int main(int argc, char* argv[]) {
    // 0. Process Runtime Program Arguments
    if (argc > 1) {
        if (stricmp(argv[1], "space") == 0) {
            g_ptt_mode = MODE_SPACEBAR;
        } else if (stricmp(argv[1], "caps") == 0) {
            g_ptt_mode = MODE_CAPSLOCK;
        } else {
            printf("Unknown option: '%s'. Valid modes: 'caps' or 'space'.\n", argv[1]);
            return 1;
        }
    }

    // 1. Load Codec System
    HMODULE hGsmDll = LoadLibraryA("libgsm_frn.dll");
    if (!hGsmDll) {
        printf("Error Code %lu: Place libgsm_frn.dll in the directory path.\n", GetLastError());
        return 1;
    }

    GsmCreateFunc gsm_create = (GsmCreateFunc)GetProcAddress(hGsmDll, "gsm_create");
    GsmDestroyFunc gsm_destroy = (GsmDestroyFunc)GetProcAddress(hGsmDll, "gsm_destroy");
    gsm_encode_ptr = (GsmEncodeFunc)GetProcAddress(hGsmDll, "gsm_encode");
    gsm_decode_ptr = (GsmDecodeFunc)GetProcAddress(hGsmDll, "gsm_decode");

    if (!gsm_create || !gsm_destroy || !gsm_encode_ptr || !gsm_decode_ptr) {
        printf("Error: Missing internal codec symbols inside DLL structure.\n");
        FreeLibrary(hGsmDll);
        return 1;
    }

    encoder_state = gsm_create();
    decoder_state = gsm_create();

    // 2. Winsock Multi-Binding Network Setup
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    net_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    BOOL reuse = TRUE;
    setsockopt(net_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    BOOL loopback = TRUE;
    setsockopt(net_socket, IPPROTO_IP, IP_MULTICAST_LOOP, (char*)&loopback, sizeof(loopback));

    // Bind to ANY adapter interface so Windows smoothly handles incoming remote traffic
    struct sockaddr_in local_addr;
    ZeroMemory(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(MULTICAST_PORT);
    bind(net_socket, (struct sockaddr*)&local_addr, sizeof(local_addr));

    // Join the logical multicast group across all available adapters 
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); 
    setsockopt(net_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

    int ttl = 1;
    setsockopt(net_socket, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, sizeof(ttl));

    ZeroMemory(&group_addr, sizeof(group_addr));
    group_addr.sin_family = AF_INET;
    group_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    group_addr.sin_port = htons(MULTICAST_PORT);

    // 3. Open Audio Hardware
    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * 2;
    wfx.cbSize = 0;

    HWAVEOUT hWaveOut;
    HWAVEIN hWaveIn;
    waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    hWaveInGlobal = hWaveIn;

    for (int i = 0; i < 3; i++) {
        ZeroMemory(&inHdrs[i], sizeof(WAVEHDR));
        inHdrs[i].lpData = (LPSTR)mic_buffers[i];
        inHdrs[i].dwBufferLength = BLOCK_PCM_SIZE;
    }

    _beginthread(receive_thread, 0, (void*)hWaveOut);
    _beginthread(transmit_thread, 0, NULL);

    printf("==================================================\n");
    printf("   FRN CODEC MULTICAST PTT SERVICE ACTIVE         \n");
    printf("==================================================\n");
    printf(" -> Multicast Group: %s:%d\n", MULTICAST_IP, MULTICAST_PORT);
    
    if (g_ptt_mode == MODE_SPACEBAR) {
        printf(" -> PTT Control: HOLD [SPACEBAR] to Transmit / Release to Listen\n\n");
    } else {
        printf(" -> PTT Control: TOGGLE [CAPS LOCK] (LED ON = Transmit / LED OFF = Listen)\n\n");
    }
    
    printf("[RX] Listening for network audio stream...\n");

    // 4. Main Interface Processing Loop
    while (1) {
        if (is_ptt_pressed()) {
            if (!bTransmitActive) {
                current_in_buf = 0;
                
                for (int i = 0; i < 3; i++) {
                    waveInPrepareHeader(hWaveInGlobal, &inHdrs[i], sizeof(WAVEHDR));
                    waveInAddBuffer(hWaveInGlobal, &inHdrs[i], sizeof(WAVEHDR));
                }
                
                 bTransmitActive = TRUE;
                printf("[TX] Transmission started...\n");
                waveInStart(hWaveInGlobal);
            }
            Sleep(20); 
        } else {
            if (bTransmitActive) {
                bTransmitActive = FALSE;
                waveInStop(hWaveInGlobal);
                waveInReset(hWaveInGlobal);
                
                for (int i = 0; i < 3; i++) {
                    if (inHdrs[i].dwFlags & WHDR_PREPARED) {
                        waveInUnprepareHeader(hWaveInGlobal, &inHdrs[i], sizeof(WAVEHDR));
                    }
                }
                printf("[RX] Listening for network audio stream...\n");
            }
            Sleep(20);
        }
    }

    gsm_destroy(encoder_state);
    gsm_destroy(decoder_state);
    FreeLibrary(hGsmDll);
    closesocket(net_socket);
    WSACleanup();
    return 0;
}

