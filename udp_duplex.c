#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h>  // Required for _kbhit() and _getch()

// Link with ws2_32.lib
#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 2048

// Helper function to convert a single hex character to its integer value
int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Function to parse a hex string into a byte array
int parse_hex_string(const char *hex_str, unsigned char *out_bytes, int max_len) {
    int len = 0;
    int i = 0;
    while (hex_str[i] != '\0' && len < max_len) {
        if (hex_str[i] == ' ' || hex_str[i] == ':') {
            i++;
            continue;
        }
        
        if (hex_str[i+1] == '\0') {
            int val = hex_char_to_val(hex_str[i]);
            if (val >= 0) {
                out_bytes[len++] = (unsigned char)val;
            }
            break;
        }

        int high = hex_char_to_val(hex_str[i]);
        int low = hex_char_to_val(hex_str[i+1]);

        if (high >= 0 && low >= 0) {
            out_bytes[len++] = (unsigned char)((high << 4) | low);
            i += 2;
        } else {
            i++;
        }
    }
    return len;
}

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET sockfd;
    struct sockaddr_in servaddr;
    struct sockaddr_in clientaddr;
    int clientaddr_len = sizeof(clientaddr);
    
    char rx_buffer[BUFFER_SIZE];
    char tx_input_buffer[BUFFER_SIZE];
    unsigned char tx_bytes[BUFFER_SIZE / 2];
    int tx_input_idx = 0;
    int has_last_client = 0;
    int port;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port_number>\n", argv[1]);
        return 1;
    }

    port = atoi(argv[1]);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "Winsock initialization failed. Error Code: %d\n", WSAGetLastError());
        return 1;
    }

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed. Error Code: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Non-blocking mode
    u_long mode = 1;
    ioctlsocket(sockfd, FIONBIO, &mode);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed. Error Code: %d\n", WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        return 1;
    }

    printf("=================================================================\n");
    printf(" UDP duplex console started on Windows port %d.\n", port);
    printf(" Press CTRL+Z followed by ENTER to completely close the app.\n");
    printf(" Incoming bytes are rendered as [HEX] and [ASCII].\n");
    printf("=================================================================\n\n");

    memset(tx_input_buffer, 0, sizeof(tx_input_buffer));

    while (1) {
        // 1. Check for network packets
        int n = recvfrom(sockfd, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&clientaddr, &clientaddr_len);
        
        if (n > 0) {
            has_last_client = 1;
            
            // Wipe current typing line visually to cleanly inject received data
            printf("\r%*s\r", tx_input_idx + 14, ""); 
            
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(clientaddr.sin_addr), ip_str, INET_ADDRSTRLEN);
            printf("\n--- Received %d bytes from %s:%d ---\n", n, ip_str, ntohs(clientaddr.sin_port));

            // FIXED: Print every byte sequentially as HEX pairs
            printf("HEX: ");
            for (int i = 0; i < n; i++) {
                printf("%02X ", (unsigned char)rx_buffer[i]);
            }
            printf("\n");

            // FIXED: Print every byte sequentially as safe ASCII text characters
            printf("ASCII: ");
            for (int i = 0; i < n; i++) {
                unsigned char c = (unsigned char)rx_buffer[i];
                // Only print legible standard characters; replace others with a dot
                if (c >= 32 && c <= 126) {
                    printf("%c", c);
                } else {
                    printf(".");
                }
            }
            printf("\n------------------------------------\n");
            fflush(stdout);

            // Restore user's current composition string
            tx_input_buffer[tx_input_idx] = '\0';
            printf("[Send Hex]: %s", tx_input_buffer);
            fflush(stdout);
        } else if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                fprintf(stderr, "\nNetwork error: %d\n", err);
                break;
            }
        }

        // 2. Handle Asynchronous Input
        if (_kbhit()) {
            char ch = _getch();

            if (ch == 26) { // CTRL+Z
                break;
            }

            if (ch == '\r' || ch == '\n') {
                tx_input_buffer[tx_input_idx] = '\0';
                
                if (tx_input_idx > 0) {
                    if (has_last_client) {
                        int tx_len = parse_hex_string(tx_input_buffer, tx_bytes, sizeof(tx_bytes));
                        if (tx_len > 0) {
                            sendto(sockfd, (const char *)tx_bytes, tx_len, 0, (struct sockaddr *)&clientaddr, clientaddr_len);
                            printf("\n -> [Sent %d bytes out]\n", tx_len);
                        } else {
                            printf("\n -> [Error: No valid hex digits parsed]\n");
                        }
                    } else {
                        printf("\n -> [Error: No remote IP address registered yet to reply to]\n");
                    }
                }
                
                tx_input_idx = 0;
                memset(tx_input_buffer, 0, sizeof(tx_input_buffer));
                printf("[Send Hex]: ");
                fflush(stdout);
            }
            else if (ch == '\b') { // Backspace
                if (tx_input_idx > 0) {
                    tx_input_idx--;
                    tx_input_buffer[tx_input_idx] = '\0';
                    printf("\b \b");
                    fflush(stdout);
                }
            }
            else if (tx_input_idx < (int)sizeof(tx_input_buffer) - 2 && ch >= 32 && ch <= 126) {
                if (tx_input_idx == 0) {
                    printf("\r[Send Hex]: ");
                }
                tx_input_buffer[tx_input_idx++] = ch;
                printf("%c", ch);
                fflush(stdout);
            }
        }

        Sleep(10);
    }

    closesocket(sockfd);
    WSACleanup();
    printf("\nListener closed successfully.\n");
    return 0;
}
