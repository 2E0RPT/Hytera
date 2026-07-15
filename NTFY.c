#include "ntfy.h"
#include <windows.h>
#include <wininet.h>
#include <stdio.h>

void SendAdminAlert(const char* message) {
    // Configuration constants
    const char* server = "ntfy.sh";
    const char* topic = "EARS-Repeater-System-Eastbourne"; // Replace with your actual ntfy channel/topic
    const char* title = "Repeater Alert";
    const char* priority = "5";               // 1=min, 2=low, 3=default, 4=high, 5=max
    const char* tags = "warning,computer";     // Comma-separated emoji tags

    // Initialize WinINet
    HINTERNET hInternet = InternetOpenA("ntfy-c-client", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        printf("InternetOpenA failed. Error: %lu\n", GetLastError());
        return;
    }

    // Connect to the ntfy server (HTTPS port 443)
    HINTERNET hConnect = InternetConnectA(hInternet, server, INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        printf("InternetConnectA failed. Error: %lu\n", GetLastError());
        InternetCloseHandle(hInternet);
        return;
    }

    // Prepare the path (e.g., "/your_secret_topic")
    char path[256];
    snprintf(path, sizeof(path), "/%s", topic);

    // Open an HTTP POST request with SSL flags
    HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", path, NULL, NULL, NULL, INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
    if (!hRequest) {
        printf("HttpOpenRequestA failed. Error: %lu\n", GetLastError());
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return;
    }

    // Build the ntfy custom configuration headers
    char headers[512];
    snprintf(headers, sizeof(headers),
             "X-Title: %s\r\n"
             "X-Priority: %s\r\n"
             "X-Tags: %s\r\n"
             "Content-Type: text/plain\r\n", 
             title, priority, tags);

    // Send the request along with the message body
    BOOL isSuccess = HttpSendRequestA(hRequest, headers, (DWORD)strlen(headers), (void*)message, (DWORD)strlen(message));

    if (isSuccess) {
        printf("Notification sent successfully!\n");
    } else {
        printf("HttpSendRequestA failed. Error: %lu\n", GetLastError());
    }

    // Clean up handles
    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
}
