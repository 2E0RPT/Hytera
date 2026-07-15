#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define TARGET_IP "192.168.1.167"
#define TARGET_PORT 162
#define BUFFER_SIZE 1024

int main() {
    WSADATA wsaData;
    SOCKET snmpSocket;
    struct sockaddr_in targetAddr, clientAddr;
    int clientAddrLen = sizeof(clientAddr);
    unsigned char rxBuffer[BUFFER_SIZE];

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("Winsock initialization failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    snmpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (snmpSocket == INVALID_SOCKET) {
        printf("Socket creation failed. Error Code: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    targetAddr.sin_family = AF_INET;
    targetAddr.sin_port = htons(TARGET_PORT);
    inet_pton(AF_INET, TARGET_IP, &targetAddr.sin_addr);

    // Updated SNMPv1 GetRequest Payload for Hytera RSSI OID: .1.3.6.1.4.1.41112.1.3.1.1.7.1
    // Read Community string: "public"
    unsigned char snmpGetRequest[] = {
        0x30, 0x2E,             // Sequence (Total packet length to follow: 46 bytes)
        0x02, 0x01, 0x00,       // Version: SNMPv1 (0x00)
        0x04, 0x06,             // Octet String (Community String Length: 6 bytes)
        'p',  'u',  'b',  'l',  'i',  'c', // Community String: "public"
        0xA0, 0x21,             // PDU Type: GetRequest (0xA0), Length: 33 bytes
        0x02, 0x04, 0x00, 0x00, 0x00, 0x01, // Request ID: 1
        0x02, 0x01, 0x00,       // Error Status: No Error (0)
        0x02, 0x01, 0x00,       // Error Index: 0
        0x30, 0x13,             // VarBind List Sequence (Length: 19 bytes)
        0x30, 0x11,             // VarBind Sequence (Length: 17 bytes)
        0x06, 0x0D,             // Object Identifier (OID Length: 13 bytes)
        // ASN.1 encoded OID: .1.3.6.1.4.1.41112.1.3.1.1.7.1
        0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xC1, 0x18, 0x01, 0x03, 0x01, 0x01, 0x07, 
        0x01,                   // Instance Index (Slot 1)
        0x05, 0x00              // Object Value: Null
    };

    printf("Sending corrected SNMPv1 GetRequest to Hytera RD985 at %s:%d...\n", TARGET_IP, TARGET_PORT);

    int txResult = sendto(snmpSocket, (char*)snmpGetRequest, sizeof(snmpGetRequest), 0,
                          (struct sockaddr*)&targetAddr, sizeof(targetAddr));

    if (txResult == SOCKET_ERROR) {
        printf("Transmission failed. Error Code: %d\n", WSAGetLastError());
        closesocket(snmpSocket);
        WSACleanup();
        return 1;
    }

    DWORD timeout = 3000;
    setsockopt(snmpSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    printf("Waiting for response (Make sure a station is actively transmitting)... \n\n");

    int rxResult = recvfrom(snmpSocket, (char*)rxBuffer, BUFFER_SIZE, 0,
                            (struct sockaddr*)&clientAddr, &clientAddrLen);

    if (rxResult == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAETIMEDOUT) {
            printf("Error: Request timed out. No response received from the repeater.\n");
        } else {
            printf("Receive failed. Error Code: %d\n", WSAGetLastError());
        }
    } else {
        printf(">>> Received %d bytes from %s <<<\n", rxResult, TARGET_IP);
        
        // Locate PDU start (Skip over Sequence, Version, and Community String)
        int pdu_offset = 2; // skip sequence header
        pdu_offset += rxBuffer[pdu_offset + 1] + 2; // skip version block
        pdu_offset += rxBuffer[pdu_offset + 1] + 2; // skip community string block
        
        // Read SNMP Error Status (Offset 4 inside the PDU block)
        int error_status = rxBuffer[pdu_offset + 6];
        
        if (error_status != 0) {
            printf("\nSNMP Error Received!\n");
            if (error_status == 2) {
                printf("Status Code: 2 (noSuchName) - The repeater firmware does not recognize this specific OID.\n");
            } else {
                printf("Status Code: %d\n", error_status);
            }
            return 0;
        }

        // Parse trailing data value if clean
        if (rxBuffer[rxResult - 3] == 0x02) {
            signed char rssi_val = (signed char)rxBuffer[rxResult - 1];
            printf("\nParsed Telemetry Success:\n");
            printf("Target OID Node: Inbound RSSI (Slot 1)\n");
            printf("Current Signal  : %d dBm\n", rssi_val);
        } else {
            printf("\nPacket received, but payload structure format was unexpected.\n");
        }
    }

    closesocket(snmpSocket);
    WSACleanup();
    return 0;
}
