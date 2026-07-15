#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>  // Added to fix the uint8_t compilation error
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define REPEATER_SNMP_PORT 161

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <repeater_ip>\n", argv[0]);
        return -1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock init failed.\n");
        return -1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        WSACleanup();
        return -1;
    }

    DWORD timeout = 3000; // 3 seconds timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REPEATER_SNMP_PORT);
    inet_pton(AF_INET, argv[1], &addr.sin_addr);

    // Hardcoded SNMP GetRequest specifically for rptChannelName.0
    // Updated to use the hidden factory default community string: "Hytera"
    uint8_t packet[] = {
        0x30, 0x2b,                                           // Sequence (Length 43)
        0x02, 0x01, 0x00,                                     // Version: v1 (0)
        0x04, 0x06, 'H', 'y', 't', 'e', 'r', 'a',             // Community text changed to: Hytera
        0xa0, 0x1e,                                           // GetRequest-PDU (Length 30)
        0x02, 0x01, 0x01,                                     // Request ID: 1
        0x02, 0x01, 0x00,                                     // Error Status: 0
        0x02, 0x01, 0x00,                                     // Error Index: 0
        0x30, 0x13,                                           // VarBind List (Length 19)
        0x30, 0x11,                                           // VarBind (Length 17)
        0x06, 0x0d,                                           // Object Identifier (Length 13)
        0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xba, 0x69, 0x01, 0x02, 0x01, 0x02, 0x0d, // OID content
        0x05, 0x00                                            // Null value
    };


    printf("[SYSTEM] Sending isolated rptChannelName query to %s...\n", argv[1]);
    int sent = sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)&addr, sizeof(addr));
    if (sent <= 0) {
        printf("[ERROR] Failed to send packet.\n");
        closesocket(sock);
        WSACleanup();
        return -1;
    }

    uint8_t buffer[2048];
    struct sockaddr_in from;
    int fromLen = sizeof(from);
    int received = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, (struct sockaddr*)&from, &fromLen);

    if (received <= 0) {
        printf("[ERROR] Timeout or no response received from repeater.\n");
    } else {
        printf("[SUCCESS] Received %d bytes from repeater!\n\n", received);
        printf("Raw Hex Output of response packet:\n");
        for (int i = 0; i < received; i++) {
            printf("%02X ", buffer[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        printf("\n\n");

        // Simple raw search loop for the string tag (0x04) near the end of the packet
        printf("Searching for text payload data inside packet...\n");
        int found_payload = 0;
        for (int i = received - 4; i > 0; i--) {
            // Find the 0x04 OctetString tag followed by a plausible length
            if (buffer[i] == 0x04 && buffer[i+1] > 0 && buffer[i+1] < 128) {
                int data_len = buffer[i+1];
                int data_start = i + 2;
                
                if (data_start + data_len <= received) {
                    printf("Found OctetString tag at byte index %d (Length field: %d bytes)\n", i, data_len);
                    printf("Decoded output text: \"");
                    
                    // Simple UTF-16LE conversion loop (skip every alternate null byte)
                    for (int k = 0; k < data_len; k += 2) {
                        char c = (char)buffer[data_start + k];
                        if (c >= 32 && c <= 126) {
                            putchar(c);
                        }
                    }
                    printf("\"\n");
                    found_payload = 1;
                    break;
                }
            }
        }
        if (!found_payload) {
            printf("[NOTICE] Could not isolate a clean 0x04 data string header in the raw response.\n");
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
