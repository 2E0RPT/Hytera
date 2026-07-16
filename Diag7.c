#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

#define REPEATER_IP "192.168.1.167" 
#define SNMP_PORT 161
#define BUFFER_SIZE 1024

int main() {
    // REM: Exact working request byte payload matched perfectly from your iReasoning Wireshark dump
    unsigned char request_bytes[] = {
        0x30, 0x2E, 0x02, 0x01, 0x00, 0x04, 0x06, 0x70, 
        0x75, 0x62, 0x6C, 0x69, 0x63, 0xA0, 0x21, 0x02, 
        0x04, 0x2E, 0x41, 0x28, 0xAF, 0x02, 0x01, 0x00, 
        0x02, 0x01, 0x00, 0x30, 0x13, 0x30, 0x11, 0x06, 
        0x0D, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 
        0x69, 0x01, 0x02, 0x04, 0x09, 0x00, 0x05, 0x00
    };
    int request_len = sizeof(request_bytes);

    // REM: Target OID byte pattern we need to look for inside the response payload
    // This represents the channel name path: 1.3.6.1.4.1.40297.1.2.4.9.0
    unsigned char target_oid[] = {
        0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xBA, 0x69, 0x01, 0x02, 0x04, 0x09, 0x00
    };
    int oid_len = sizeof(target_oid);

    // REM: Dedicated string array variable box to safely hold the clean text for the future GUI Text Box
    char channel_name_str[64];
    memset(channel_name_str, 0, sizeof(channel_name_str)); // Clear out array with clean nulls

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to init Winsock.\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("Could not create socket.\n");
        WSACleanup();
        return 1;
    }

    // REM: 3-second timeout rule to keep the program from freezing if connection drops
    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    struct sockaddr_in repeater_addr;
    memset(&repeater_addr, 0, sizeof(repeater_addr));
    repeater_addr.sin_family = AF_INET;
    repeater_addr.sin_port = htons(SNMP_PORT);
    repeater_addr.sin_addr.s_addr = inet_addr(REPEATER_IP);

    printf("Sending working iReasoning packet...\n");
    if (sendto(sock, (char*)request_bytes, request_len, 0, (struct sockaddr*)&repeater_addr, sizeof(repeater_addr)) == SOCKET_ERROR) {
        printf("Send failed.\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    unsigned char response_buffer[BUFFER_SIZE];
    struct sockaddr_in from_addr;
    int from_len = sizeof(from_addr);

    printf("Waiting for response...\n");
    int response_len = recvfrom(sock, (char*)response_buffer, BUFFER_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);

    if (response_len == SOCKET_ERROR) {
        printf("No response received from repeater.\n");
    } else {
        printf("Received %d bytes back!\n", response_len);

        // REM: PARSING LOGIC FOR HYTERAGUI1-10 
        // 1. Search the raw buffer to find where the target OID path structure ends
        unsigned char *oid_ptr = NULL;
        for (int i = 0; i <= response_len - oid_len; i++) {
            if (memcmp(&response_buffer[i], target_oid, oid_len) == 0) {
                oid_ptr = &response_buffer[i];
                break;
            }
        }

        if (oid_ptr != NULL) {
            unsigned char *data_tag_ptr = oid_ptr + oid_len;

            // REM: 0x04 confirms an Octet String type payload follows
            if (*data_tag_ptr == 0x04) {
                int raw_bytes_len = *(data_tag_ptr + 1); // Total byte length of the string container
                unsigned char *string_start = data_tag_ptr + 2; 

                // REM: FIXED FOR UTF-16 ENCODING TRUNCATION
                // Because every character is 2 bytes wide, we loop through step-by-step
                // skipping the empty high-order null bytes to construct a safe single-byte string.
                int write_index = 0;
                for (int j = 0; j < raw_bytes_len; j += 2) {
                    // Safety check to ensure we never overrun our local text array storage bounds
                    if (write_index < (int)sizeof(channel_name_str) - 1) {
                        channel_name_str[write_index] = string_start[j];
                        write_index++;
                    }
                }
                channel_name_str[write_index] = '\0'; // Force a clean string null-termination marker at the real end

                printf("\n----------------------------------------\n");
                printf("Extracted Channel String: \"%s\"\n", channel_name_str);
                printf("----------------------------------------\n");
            } else {
                printf("Error: Response element type payload was not an expected Octet String (0x04).\n");
            }
        } else {
            printf("Error: Could not locate the target channel OID structure pattern inside the data packet.\n");
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
