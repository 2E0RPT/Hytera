#include <winsock2.h> // 1. Winsock2 MUST come first
#include <windows.h>  // 2. Windows core header next
#include <stdio.h>
#include <stdlib.h>
#include "telemetry.h"


#define PORT 8080
#define WM_UPDATE_TELEMETRY (WM_USER + 1) // Custom Windows Event Message

// Global UI Handles
HWND hChannelLabel, hVoltageLabel, hTempLabel;
SOCKET sockfd = INVALID_SOCKET;
HANDLE hNetThread = NULL;

// Helper function to extract text strings out of UTF-16LE data blocks
void decode_utf16le(const char *utf16_str, size_t max_bytes, char *out_ascii) {
    size_t out_idx = 0;
    for (size_t i = 0; i < max_bytes - 1; i += 2) {
        if (utf16_str[i] == 0 && utf16_str[i+1] == 0) break;
        if (utf16_str[i] >= 32 && utf16_str[i] <= 126) {
            out_ascii[out_idx++] = utf16_str[i];
        }
    }
    out_ascii[out_idx] = '\0';
}

// Background Network Worker Thread
DWORD WINAPI NetworkReceiverThread(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    struct sockaddr_in server_addr, client_addr;
    int addr_len = sizeof(client_addr);
    
    // Allocate static heap memory so data remains safe across thread post boundaries
    RepeaterTelemetry* incoming_packet = malloc(sizeof(RepeaterTelemetry));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        OutputDebugStringA("Winsock GUI Bind Failure.\n");
        free(incoming_packet);
        return 1;
    }

    while (1) {
        int bytes = recvfrom(sockfd, (char *)incoming_packet, sizeof(RepeaterTelemetry), 0, 
                             (struct sockaddr *)&client_addr, &addr_len);
        
        if (bytes == sizeof(RepeaterTelemetry)) {
            // Post notification safely to the main GUI thread message pump
            PostMessage(hWnd, WM_UPDATE_TELEMETRY, 0, (LPARAM)incoming_packet);
            
            // Re-allocate buffer slot for subsequent packet deliveries
            incoming_packet = malloc(sizeof(RepeaterTelemetry));
        }
    }
    return 0;
}

// GUI Window Event Handling Loop
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Layout Static Text Containers inside HyteraGUI1-4 Window frame
            CreateWindow("STATIC", "Hytera Repeater Live Telemetry", WS_VISIBLE | WS_CHILD, 20, 10, 300, 20, hWnd, NULL, NULL, NULL);
            hChannelLabel = CreateWindow("STATIC", "Channel Name : Waiting...", WS_VISIBLE | WS_CHILD, 20, 45, 350, 20, hWnd, NULL, NULL, NULL);
            hVoltageLabel = CreateWindow("STATIC", "Voltage      : Waiting...", WS_VISIBLE | WS_CHILD, 20, 70, 350, 20, hWnd, NULL, NULL, NULL);
            hTempLabel    = CreateWindow("STATIC", "Temperature  : Waiting...", WS_VISIBLE | WS_CHILD, 20, 95, 350, 20, hWnd, NULL, NULL, NULL);
            break;
        }
        case WM_UPDATE_TELEMETRY: {
            RepeaterTelemetry* data = (RepeaterTelemetry*)lParam;
            char clean_name[32];
            char UI_buffer[64];

            // 1. Process decoded channel name string parsed via Hytera OID
            decode_utf16le(data->channel_name, sizeof(data->channel_name), clean_name);
            sprintf(UI_buffer, "Channel Name : %s", clean_name);
            SetWindowText(hChannelLabel, UI_buffer);

            // 2. Render untouched numeric metric properties safely
            sprintf(UI_buffer, "Voltage      : %.2f V", data->voltage);
            SetWindowText(hVoltageLabel, UI_buffer);

            sprintf(UI_buffer, "Temperature  : %.2f C", data->temperature);
            SetWindowText(hTempLabel, UI_buffer);

            free(data); // Free the memory allocated by the worker thread
            break;
        }
        case WM_DESTROY:
            if (sockfd != INVALID_SOCKET) closesocket(sockfd);
            WSACleanup();
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WSADATA wsaData;
    WNDCLASS wc = {0};
    HWND hWnd;
    MSG msg;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    // Open background UDP socket interface
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == INVALID_SOCKET) { WSACleanup(); return 1; }

    // Register Win32 GUI Class window structure 
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "HyteraGUI14Class";
    RegisterClass(&wc);

    hWnd = CreateWindow("HyteraGUI14Class", "HyteraGUI v1.4", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 
                        CW_USEDEFAULT, CW_USEDEFAULT, 420, 180, NULL, NULL, hInstance, NULL);

    // Launch background thread to isolate UDP blocking mechanics away from message pump
    hNetThread = CreateThread(NULL, 0, NetworkReceiverThread, (LPVOID)hWnd, 0, NULL);

    // Main App UI Non-blocking Message Pump loop Execution
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
