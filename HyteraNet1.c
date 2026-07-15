#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>

// Link with ws2_32.lib
#pragma comment(lib, "ws2_32.lib")

#define REPEATER_IP   "192.168.1.100" // Replace with target RD985 IP address
#define REPEATER_PORT 50000           // Default Hytera network media/data port

// Placeholder structure representing a basic Hytera network data payload frame
typedef struct {
    unsigned char protocol_id;
    unsigned char message_type;
    unsigned short sequence_num;
    unsigned char dmr_payload[33]; 
} HyteraFrame;

int main() {
    WSADATA wsaData;
    SOCKET clientSocket = INVALID_SOCKET;
    struct sockaddr_in repeaterAddr;
    int result;

    // 1. Initialize Winsock
    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("WSAStartup failed with error: %d\n", result);
        return 1;
    }

    // 2. Create a UDP socket for network transmission
    clientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (clientSocket == INVALID_SOCKET) {
        printf("Socket creation failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Configure target address architecture
    repeaterAddr.sin_family = AF_INET;
    repeaterAddr.sin_port = htons(REPEATER_PORT);
    inet_pton(AF_INET, REPEATER_IP, &repeaterAddr.sin_addr);

    // Set socket to non-blocking mode to allow simultaneous transmission and reception
    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);

    printf("Socket initialized. Directing packets to %s:%d...\n", REPEATER_IP, REPEATER_PORT);

    // 3. Craft registration handshake frame (simulated packet structure)
    HyteraFrame registerPacket;
    registerPacket.protocol_id = 0x90;  // Standard Hytera proprietary framing tag
    registerPacket.message_type = 0x01; // Registration/Handshake type field
    registerPacket.sequence_num = htons(1);
    memset(registerPacket.dmr_payload, 0, sizeof(registerPacket.dmr_payload));

    // Transmit handshake frame to initiate connection
    result = sendto(clientSocket, (const char*)&registerPacket, sizeof(registerPacket), 0,
                    (struct sockaddr*)&repeaterAddr, sizeof(repeaterAddr));
    if (result == SOCKET_ERROR) {
        printf("Transmission failed with error: %d\n", WSAGetLastError());
    } else {
        printf("Registration handshake sent successfully (%d bytes).\n", result);
    }

    // 4. Infinite Execution loop processing continuous Transmit/Receive routines
    char rxBuffer[512];
    struct sockaddr_in senderAddr;
    int senderAddrLen = sizeof(senderAddr);

    printf("Listening for network port stream. Press Ctrl+C to terminate...\n");
    while (1) {
        // Read incoming UDP socket buffers asynchronously 
        result = recvfrom(clientSocket, rxBuffer, sizeof(rxBuffer), 0,
                          (struct sockaddr*)&senderAddr, &senderAddrLen);

        if (result > 0) {
            printf("Received %d bytes from repeater: ", result);
            for (int i = 0; i < result; i++) {
                printf("%02X ", (unsigned char)rxBuffer[i]);
            }
            printf("\n");
            
            // Handle parsing logic here (e.g., parsing AMBE+2 vocoder data packets)
        } 
        else if (result == SOCKET_ERROR) {
            int errorCode = WSAGetLastError();
            // WSAEWOULDBLOCK indicates no data is waiting in the network queue
            if (errorCode != WSAEWOULDBLOCK) {
                printf("Reception failure encountered. Error code: %d\n", errorCode);
            }
        }

        // Avoid pegged CPU scheduling execution
        Sleep(10);
    }

    // Cleanup resources
    closesocket(clientSocket);
    WSACleanup();
    return 0;
}
