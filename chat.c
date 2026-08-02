#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h> // For _beginthreadex

// Link with ws2_32.lib
#pragma comment(lib, "ws2_32.lib")

#define PORT 10525
#define BUFFER_SIZE 1024

// Receiver Thread Function
unsigned __stdcall receive_thread(void* param) {
    SOCKET sock = *(SOCKET*)param;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in from_addr;
    int from_len = sizeof(from_addr);

    while (1) {
        int bytes_received = recvfrom(sock, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr*)&from_addr, &from_len);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            
            // Print the incoming message along with the sender's IP address
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));
            printf("\r[%s]: %s\nYou: ", ip_str, buffer);
            fflush(stdout);
        } else if (bytes_received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEINTR) { // Ignore intentional closure overrides
                printf("\nReceive error: %d\n", err);
            }
            break;
        }
    }
    return 0;
}

int main() {
    WSADATA wsa_data;
    SOCKET sock;
    struct sockaddr_in local_addr;
    struct sockaddr_in broadcast_addr;
    int opt_true = 1;

    // 1. Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    // 2. Create a UDP Socket
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed. Error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // 3. CRITICAL: Allow port reuse to avoid "Address already in use" bind errors
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt_true, sizeof(opt_true)) == SOCKET_ERROR) {
        printf("Setsockopt(SO_REUSEADDR) failed. Error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 4. Enable Broadcast permissions on the socket
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&opt_true, sizeof(opt_true)) == SOCKET_ERROR) {
        printf("Setsockopt(SO_BROADCAST) failed. Error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 5. Bind the socket to the port (Listen to everything)
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces
    local_addr.sin_port = htons(PORT);

    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
        printf("Bind failed. Error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Configure the target broadcast address destinations
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255"); // Limited local broadcast
    broadcast_addr.sin_port = htons(PORT);

    printf("=== Local UDP Broadcast Chat Room ===\n");
    printf("Listening on port %d...\n", PORT);
    printf("Type your message and press Enter.\n\n");

    // 6. Spin up background thread to handle asynchronous incoming text
    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, receive_thread, &sock, 0, NULL);

    // 7. Main loop for sending user input
    char message[BUFFER_SIZE];
    while (1) {
        printf("You: ");
        fflush(stdout);
        
        if (fgets(message, sizeof(message), stdin) == NULL) break;

        // Strip the trailing newline character
        message[strcspn(message, "\n")] = '\0';

        // Exit command
        if (strcmp(message, "exit") == 0) {
            break;
        }

        // Only send if the user typed text
        if (strlen(message) > 0) {
            int bytes_sent = sendto(sock, message, (int)strlen(message), 0, 
                                    (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
            if (bytes_sent == SOCKET_ERROR) {
                printf("Send failed. Error: %d\n", WSAGetLastError());
            }
        }
    }

    // Cleanup resources
    closesocket(sock);
    CloseHandle(hThread);
    WSACleanup();
    return 0;
}
