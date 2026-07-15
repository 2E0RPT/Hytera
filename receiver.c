#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h> // Windows Sockets Header
#include "telemetry.h"

#define PORT 8080

void print_utf16le_as_ascii(const char *utf16_str, size_t max_bytes) {
    for (size_t i = 0; i < max_bytes - 1; i += 2) {
        if (utf16_str[i] == 0 && utf16_str[i+1] == 0) {
            break;
        }
        if (utf16_str[i] >= 32 && utf16_str[i] <= 126) {
            putchar(utf16_str[i]);
        }
    }
}

int main() {
    WSADATA wsaData;
    SOCKET sockfd;
    struct sockaddr_in server_addr, client_addr;
    int addr_len = sizeof(client_addr);
    RepeaterTelemetry telemetry_packet;

    // Initialize Winsock (Mandatory for Windows)
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("Winsock initialization failed. Error: %d\n", WSAGetLastError());
        return 1;
    }

    // Create UDP Socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        printf("Socket creation failed. Error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(PORT);

    // Bind port
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed. Error: %d\n", WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        return 1;
    }

    printf("Telemetry Receiver Running on Windows port %d...\n", PORT);

    while (1) {
        int bytes_received = recvfrom(sockfd, (char *)&telemetry_packet, sizeof(RepeaterTelemetry), 0, 
                                      (struct sockaddr *)&client_addr, &addr_len);
        
        if (bytes_received == sizeof(RepeaterTelemetry)) {
            printf("\n--- New Telemetry Frame From %s:%d ---\n", 
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            printf("Device ID    : %u\n", telemetry_packet.device_id);
            printf("Channel Name : ");
            print_utf16le_as_ascii(telemetry_packet.channel_name, sizeof(telemetry_packet.channel_name));
            printf("\n");
            printf("Voltage      : %.2f V\n", telemetry_packet.voltage);
            printf("Temperature  : %.2f C\n", telemetry_packet.temperature);
        }
    }

    closesocket(sockfd);
    WSACleanup();
    return 0;
}
