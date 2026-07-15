#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h> // Windows Sockets Header
#include "telemetry.h"

#define PORT 8080

int main() {
    WSADATA wsaData;
    SOCKET sockfd;
    struct sockaddr_in server_addr;
    RepeaterTelemetry packet;

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

    // Populate data fields
    packet.device_id = 101;
    packet.voltage = 13.8f;     
    packet.temperature = 42.5f; 

    // Raw UTF-16LE data from Hytera OID: .1.3.6.1.4.1.40297.1.2.4.9.0
    unsigned char hytera_oid_data[] = {
        0x44, 0x00, 0x49, 0x00, 0x47, 0x00, 0x49, 0x00, 
        0x54, 0x00, 0x41, 0x00, 0x4C, 0x00, 0x20, 0x00, 
        0x53, 0x00, 0x6C, 0x00, 0x6F, 0x00, 0x74, 0x00, 
        0x20, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(packet.channel_name, hytera_oid_data, sizeof(packet.channel_name));

    // Send payload
    sendto(sockfd, (char *)&packet, sizeof(RepeaterTelemetry), 0, 
           (struct sockaddr *)&server_addr, sizeof(server_addr));

    printf("Telemetry packet containing Hytera OID data transmitted successfully from Windows.\n");

    closesocket(sockfd);
    WSACleanup();
    return 0;
}
