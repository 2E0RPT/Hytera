/*
 * hytera_bbs_chat.c
 *
 * An 80's-BBS-style chat display for the Hytera TMS proxy system built in
 * hytera_tms_responder.c. Shows a scrolling, colour-coded message window
 * with a reserved input line ("> " prompt) at the bottom, and a fixed
 * light-blue system banner on the top-left row reminding how to quit.
 *
 * Build (MinGW / gcc on Windows 10):
 *      gcc hytera_bbs_chat.c -o hytera_bbs_chat.exe -lws2_32
 *
 * Run:
 *      hytera_bbs_chat.exe <RPT_ID>
 *      hytera_bbs_chat.exe 200
 *
 * Typing at the bottom prompt uses "{destination} {message}", e.g.
 *      203 Are you there?
 * which gets sent on UDP proxy port 10252 as:
 *      [SMS-TS1] {RPT_ID} to {destination} > {message}\r\n
 * e.g. with RPT_ID 200:
 *      [SMS-TS1] 200 to 203 > Are you there?
 *
 * A destination written as "TG{n}" (case-insensitive) sends to a
 * talkgroup instead, e.g.:
 *      TG1 hello everyone
 * which sends "[SMS-TS1] {RPT_ID} to TG{n} > {message}\r\n". There's no
 * PENDING/DELIVERED/FAILED line for this locally - group sends have no
 * such lifecycle - but you'll see it echo back in cyan via
 * proxy_group_thread once the broadcast loops back, which confirms it
 * went out.
 *
 * Typing "/exit" or "/EXIT" (case-insensitive) and pressing Enter quits
 * the program immediately - it is handled entirely locally and is never
 * sent out over UDP.
 *
 * --------------------------------------------------------------------
 * Ports (must match hytera_tms_responder.c's LAN-facing conventions -
 * SO_REUSEADDR + SO_BROADCAST, bound INADDR_ANY, matching the user's
 * reference chat.c):
 *
 *   10525 (DEVLOG_PORT) - read here. One-way event feed from the
 *          responder:
 *            [SMS-TS1] RX > {sender} > {dest} > SMS={n} > {message}
 *                      (older responder builds without {dest} are also
 *                      accepted and treated as addressed to us)
 *            [SMS-TS1] TX > PROXY > SMS={n} > {message}
 *            [SMS-TS1] TRY > PROXY > SMS={n} > {message}
 *            [SMS-TS1] PASS > PROXY > SMS={n} > {message}
 *            [SMS-TS1] FAIL > PROXY > SMS={n} Failed or unconfirmed.
 *
 *   10252 (PROXY_PORT) - both read and write. Typing "{destination}
 *          {message}" at the input prompt and pressing Enter sends
 *          "[SMS-TS1] {RPT_ID} to {destination} > {message}\r\n" here.
 *          The resulting TX/TRY/PASS/FAIL lifecycle is then picked up
 *          via the devlog port above, same as any other proxy send.
 *          This port is also read here for group (talkgroup) messages -
 *          "[SMS-TS1] {sender} to TG{n} > {message}" - which the
 *          responder only ever announces on this port, never on the
 *          devlog feed. Shown in cyan as "{sender} > TG{n}: {message}",
 *          with no PENDING/DELIVERED/FAILED lifecycle since group sends
 *          don't have delivery-status tracking (see
 *          hytera_tms_responder.c). Sending TO a talkgroup from this
 *          program's own input line isn't wired up yet - only display.
 *
 * Display rules (as specified):
 *   RX > {sender} > {dest} > {message}          ->  if RPT_ID is the sender or dest:
 *                                                       green: {sender} > {message}
 *                                                    otherwise (a private message between
 *                                                    two OTHER radios, relayed to us only
 *                                                    because the responder is monitoring):
 *                                                       grey: {sender} to {dest} > {message}
 *   TX > PROXY > SMS={n} > {message}            ->  yellow: YOU > PENDING 1 > {message}
 *   TRY > PROXY > SMS={n} > {message}           ->  yellow: YOU > PENDING {attempt} > {message}
 *                                                    (overwrites the same on-screen line,
 *                                                     matched by SMS number; attempt count
 *                                                     is tracked locally since the wire
 *                                                     events don't carry it)
 *   PASS > PROXY > SMS={n} > {message}          ->  green: YOU > DELIVERED {dest} > {message}
 *                                                    (overwrites the same line; {dest} is
 *                                                     shown as "?" until the send feature
 *                                                     exists to record it - see note above)
 *   FAIL > PROXY > SMS={n} ...                  ->  red: YOU > FAILED > {message}
 *                                                    (overwrites the same line; message text
 *                                                     is recalled from the original TX/TRY,
 *                                                     since the FAIL event itself doesn't
 *                                                     repeat it)
 * --------------------------------------------------------------------
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define DEVLOG_PORT   10525
#define PROXY_PORT    10252

#define MAX_LINES     500
#define MAX_TEXT      300
#define MAX_MESSAGE   220
#define MAX_INPUT     256

#define COLOR_DEFAULT   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COLOR_GREEN     (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_YELLOW    (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COLOR_RED       (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COLOR_LIGHTBLUE (FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COLOR_CYAN      (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COLOR_GREY      (FOREGROUND_INTENSITY)

#define SYSTEM_BANNER "[SYSTEM] Type /EXIT to exit the program."

typedef struct {
    int used;
    int has_sms;             /* 1 while this line can still be updated (PENDING) */
    unsigned int sms_number;
    int attempt;
    char message[MAX_MESSAGE]; /* the actual radio message text, remembered
                                   across TX -> TRY -> PASS/FAIL updates */
    char text[MAX_TEXT];       /* the fully-formatted line as displayed */
    WORD color;
} chat_line_t;

static CRITICAL_SECTION g_lock;

static chat_line_t g_lines[MAX_LINES];
static int g_line_count = 0;

static HANDLE g_hConsole;
static int g_console_width;
static int g_win_top;
static int g_msg_top;    /* first row of the scrolling message area (below the banner) */
static int g_msg_rows;
static int g_input_row;

static char g_input_buffer[MAX_INPUT];
static int g_input_len = 0;

static SOCKET g_devlog_sock;
static SOCKET g_proxy_sock;                 /* used by send_proxy_message() */
static struct sockaddr_in g_proxy_target;    /* 255.255.255.255:10252 */
static unsigned int g_rpt_id;                /* our station ID, from argv[1] */

/* ---- low-level console drawing (direct buffer writes - never moves the
   cursor or triggers a scroll, unlike printf/WriteConsole) ---- */

static void clear_row(int row, WORD color) {
    COORD coord;
    DWORD written;
    coord.X = 0;
    coord.Y = (SHORT)row;
    FillConsoleOutputCharacterA(g_hConsole, ' ', g_console_width, coord, &written);
    FillConsoleOutputAttribute(g_hConsole, color, g_console_width, coord, &written);
}

static void draw_row_text(int row, const char *text, WORD color) {
    COORD coord;
    DWORD written;
    int len = (int)strlen(text);
    if (len > g_console_width) len = g_console_width;

    coord.X = 0;
    coord.Y = (SHORT)row;
    clear_row(row, color);
    WriteConsoleOutputCharacterA(g_hConsole, text, len, coord, &written);
}

/* Redraws the reserved bottom input line and parks the blinking cursor
   at the end of whatever's currently typed. */
static void draw_input_prompt(void) {
    char line[MAX_INPUT + 4];

    EnterCriticalSection(&g_lock);
    _snprintf(line, sizeof(line), "> %s", g_input_buffer);
    draw_row_text(g_input_row, line, COLOR_DEFAULT);
    {
        COORD c;
        c.X = (SHORT)(2 + g_input_len);
        c.Y = (SHORT)g_input_row;
        SetConsoleCursorPosition(g_hConsole, c);
    }
    LeaveCriticalSection(&g_lock);
}

/* Redraws the whole message window from g_lines[], then restores the
   input line/cursor. */
static void redraw_messages(void) {
    int start, row;

    EnterCriticalSection(&g_lock);
    start = g_line_count - g_msg_rows;
    if (start < 0) start = 0;

    for (row = 0; row < g_msg_rows; row++) {
        int idx = start + row;
        if (idx < g_line_count) {
            draw_row_text(g_msg_top + row, g_lines[idx].text, g_lines[idx].color);
        } else {
            clear_row(g_msg_top + row, COLOR_DEFAULT);
        }
    }
    LeaveCriticalSection(&g_lock);

    draw_input_prompt();
}

/* ---- chat line storage ---- */

static void append_line(const char *text, WORD color, int has_sms,
                         unsigned int sms_number, int attempt, const char *message) {
    chat_line_t *p;

    EnterCriticalSection(&g_lock);
    if (g_line_count == MAX_LINES) {
        memmove(&g_lines[0], &g_lines[1], sizeof(chat_line_t) * (MAX_LINES - 1));
        g_line_count--;
    }
    p = &g_lines[g_line_count++];
    p->used = 1;
    p->has_sms = has_sms;
    p->sms_number = sms_number;
    p->attempt = attempt;
    if (message) {
        strncpy(p->message, message, sizeof(p->message) - 1);
        p->message[sizeof(p->message) - 1] = '\0';
    } else {
        p->message[0] = '\0';
    }
    strncpy(p->text, text, sizeof(p->text) - 1);
    p->text[sizeof(p->text) - 1] = '\0';
    p->color = color;
    LeaveCriticalSection(&g_lock);

    redraw_messages();
}

/* Finds the most recent still-updatable (has_sms) line for an SMS number.
   Returns NULL if none found. Caller must hold g_lock. */
static chat_line_t *find_pending_line(unsigned int sms_number) {
    int i;
    for (i = g_line_count - 1; i >= 0; i--) {
        if (g_lines[i].has_sms && g_lines[i].sms_number == sms_number) {
            return &g_lines[i];
        }
    }
    return NULL;
}

/* ---- devlog (port 10525) event handling ---- */

static void handle_rx(unsigned int sender, unsigned int dest, const char *message) {
    char text[MAX_TEXT];
    WORD color;

    if (sender == g_rpt_id || dest == g_rpt_id) {
        color = COLOR_GREEN;
        _snprintf(text, sizeof(text), "%u > %s", sender, message);
    } else {
        /* a private message between two OTHER radios, relayed to us
           purely because the responder is monitoring - not "ours", so
           shown dimmed with the full sender/dest context instead of the
           usual "{sender} > {message}" shorthand */
        color = COLOR_GREY;
        _snprintf(text, sizeof(text), "%u to %u > %s", sender, dest, message);
    }

    append_line(text, color, 0, 0, 0, message);
}

static void handle_tx(unsigned int sms, const char *message) {
    char text[MAX_TEXT];
    _snprintf(text, sizeof(text), "YOU > PENDING 1 > %s", message);
    append_line(text, COLOR_YELLOW, 1, sms, 1, message);
}

static void handle_try(unsigned int sms, const char *message) {
    char text[MAX_TEXT];
    int attempt;

    EnterCriticalSection(&g_lock);
    {
        chat_line_t *p = find_pending_line(sms);
        if (p) {
            p->attempt++;
            attempt = p->attempt;
            _snprintf(text, sizeof(text), "YOU > PENDING %d > %s", attempt, p->message);
            strncpy(p->text, text, sizeof(p->text) - 1);
            p->text[sizeof(p->text) - 1] = '\0';
            p->color = COLOR_YELLOW;
            LeaveCriticalSection(&g_lock);
            redraw_messages();
            return;
        }
    }
    LeaveCriticalSection(&g_lock);

    /* no matching TX seen (e.g. started this client mid-conversation) -
       show what we can. */
    _snprintf(text, sizeof(text), "YOU > PENDING 2 > %s", message);
    append_line(text, COLOR_YELLOW, 1, sms, 2, message);
}

static void handle_pass(unsigned int sms, const char *message) {
    char text[MAX_TEXT];

    EnterCriticalSection(&g_lock);
    {
        chat_line_t *p = find_pending_line(sms);
        if (p) {
            /* destination isn't carried by the PASS event yet - shown as
               "?" until the send feature exists to record it (see the
               file header note) */
            _snprintf(text, sizeof(text), "YOU > DELIVERED ? > %s", p->message);
            strncpy(p->text, text, sizeof(p->text) - 1);
            p->text[sizeof(p->text) - 1] = '\0';
            p->color = COLOR_GREEN;
            p->has_sms = 0; /* delivered - no further updates expected */
            LeaveCriticalSection(&g_lock);
            redraw_messages();
            return;
        }
    }
    LeaveCriticalSection(&g_lock);

    _snprintf(text, sizeof(text), "YOU > DELIVERED ? > %s", message);
    append_line(text, COLOR_GREEN, 0, sms, 0, message);
}

static void handle_fail(unsigned int sms) {
    char text[MAX_TEXT];

    EnterCriticalSection(&g_lock);
    {
        chat_line_t *p = find_pending_line(sms);
        if (p) {
            _snprintf(text, sizeof(text), "YOU > FAILED > %s", p->message);
            strncpy(p->text, text, sizeof(p->text) - 1);
            p->text[sizeof(p->text) - 1] = '\0';
            p->color = COLOR_RED;
            p->has_sms = 0; /* failed - terminal */
            LeaveCriticalSection(&g_lock);
            redraw_messages();
            return;
        }
    }
    LeaveCriticalSection(&g_lock);

    _snprintf(text, sizeof(text), "YOU > FAILED > (message text unknown)");
    append_line(text, COLOR_RED, 0, sms, 0, "");
}

static void handle_devlog_line(char *line) {
    unsigned int sender, dest, sms;
    char msg[MAX_MESSAGE];
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }

    if (sscanf(line, "[SMS-TS1] RX > %u > %u > SMS=%u > %219[^\r\n]", &sender, &dest, &sms, msg) == 4) {
        handle_rx(sender, dest, msg);
    } else if (sscanf(line, "[SMS-TS1] RX > %u > SMS=%u > %219[^\r\n]", &sender, &sms, msg) == 3) {
        /* older responder without the {dest} field - assume it's for us */
        handle_rx(sender, g_rpt_id, msg);
    } else if (sscanf(line, "[SMS-TS1] RX > %u > %219[^\r\n]", &sender, msg) == 2) {
        handle_rx(sender, g_rpt_id, msg);
    } else if (sscanf(line, "[SMS-TS1] TX > PROXY > SMS=%u > %219[^\r\n]", &sms, msg) == 2) {
        handle_tx(sms, msg);
    } else if (sscanf(line, "[SMS-TS1] TRY > PROXY > SMS=%u > %219[^\r\n]", &sms, msg) == 2) {
        handle_try(sms, msg);
    } else if (sscanf(line, "[SMS-TS1] PASS > PROXY > SMS=%u > %219[^\r\n]", &sms, msg) == 2) {
        handle_pass(sms, msg);
    } else if (sscanf(line, "[SMS-TS1] FAIL > PROXY > SMS=%u", &sms) == 1) {
        handle_fail(sms);
    }
    /* anything else: not one of our events, ignore */
}

static unsigned __stdcall devlog_thread(void *unused) {
    unsigned char rbuf[1500];
    (void)unused;

    for (;;) {
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = recvfrom(g_devlog_sock, (char *)rbuf, sizeof(rbuf) - 1, 0,
                          (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            rbuf[n] = '\0';
            handle_devlog_line((char *)rbuf);
        }
    }
    return 0;
}

/*
 * Group (talkgroup) messages aren't part of the devlog (10525) feed at
 * all - the responder only ever announces them on the proxy port
 * (10252) as "[SMS-TS1] {sender} to TG{n} > {message}". g_proxy_sock is
 * already bound to that port (for sending), so it can also be read from
 * here to pick those up. Shown in cyan, one line per message - no
 * PENDING/DELIVERED/FAILED tracking like private YOU-sent messages get,
 * since group sends don't have that lifecycle (see hytera_tms_responder.c's
 * notes on why group sends are fire-and-forget).
 */
static void handle_proxy_group_line(char *line) {
    unsigned int sender, talkgroup;
    char msg[MAX_MESSAGE];
    char text[MAX_TEXT];
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }

    if (sscanf(line, "[SMS-TS1] %u to TG%u > %219[^\r\n]", &sender, &talkgroup, msg) != 3) {
        return; /* not a group line (private lines are handled via the devlog feed instead) */
    }

    _snprintf(text, sizeof(text), "%u > TG%u: %s", sender, talkgroup, msg);
    append_line(text, COLOR_CYAN, 0, 0, 0, msg);
}

static unsigned __stdcall proxy_group_thread(void *unused) {
    unsigned char rbuf[1500];
    (void)unused;

    for (;;) {
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = recvfrom(g_proxy_sock, (char *)rbuf, sizeof(rbuf) - 1, 0,
                          (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            rbuf[n] = '\0';
            handle_proxy_group_line((char *)rbuf);
        }
    }
    return 0;
}

/* Sends "[SMS-TS1] {RPT_ID} to {destination} > {message}\r\n" on the
   proxy port. The resulting TX/TRY/PASS/FAIL lifecycle is picked up and
   displayed via the devlog thread, same as any other proxy send - we
   don't add a line here ourselves to avoid a duplicate. */
static void send_proxy_message(unsigned int destination, const char *message) {
    char line[MAX_TEXT];
    int len = _snprintf(line, sizeof(line), "[SMS-TS1] %u to %u > %s\r\n",
                         g_rpt_id, destination, message);
    if (len < 0) len = (int)sizeof(line) - 1;
    sendto(g_proxy_sock, line, len, 0,
           (struct sockaddr *)&g_proxy_target, sizeof(g_proxy_target));
}

/* Sends "[SMS-TS1] {RPT_ID} to TG{talkgroup} > {message}\r\n" - the
   group equivalent of send_proxy_message(). Unlike a private send, this
   never shows a PENDING/DELIVERED/FAILED line locally, because group
   sends have no such lifecycle (see hytera_tms_responder.c). What you'll
   see instead is this same line echoing back in cyan via
   proxy_group_thread once the broadcast loops back - that's normal and
   confirms it went out, not a duplicate. */
static void send_proxy_group_message(unsigned int talkgroup, const char *message) {
    char line[MAX_TEXT];
    int len = _snprintf(line, sizeof(line), "[SMS-TS1] %u to TG%u > %s\r\n",
                         g_rpt_id, talkgroup, message);
    if (len < 0) len = (int)sizeof(line) - 1;
    sendto(g_proxy_sock, line, len, 0,
           (struct sockaddr *)&g_proxy_target, sizeof(g_proxy_target));
}

/* Parses the input line as either "TG{n} {message}" (group send) or
   "{destination} {message}" (private send). On success, sends it and
   returns 1. On a malformed line, shows a local system note and
   returns 0. */
static int try_send_input(const char *input) {
    unsigned int destination;
    unsigned int talkgroup;
    int consumed = 0;
    const char *message;

    if ((input[0] == 'T' || input[0] == 't') && (input[1] == 'G' || input[1] == 'g') &&
        sscanf(input + 2, "%u%n", &talkgroup, &consumed) == 1 && consumed > 0) {
        message = input + 2 + consumed;
        while (*message == ' ') message++;

        if (*message == '\0') {
            append_line("(no message text after the talkgroup)",
                        COLOR_DEFAULT, 0, 0, 0, NULL);
            return 0;
        }

        send_proxy_group_message(talkgroup, message);
        return 1;
    }

    if (sscanf(input, "%u%n", &destination, &consumed) != 1 || consumed == 0) {
        append_line("(format: {destination} {message} or TG{n} {message}, e.g. 203 Are you there? / TG1 hello everyone)",
                    COLOR_DEFAULT, 0, 0, 0, NULL);
        return 0;
    }

    message = input + consumed;
    while (*message == ' ') message++;

    if (*message == '\0') {
        append_line("(no message text after the destination)",
                    COLOR_DEFAULT, 0, 0, 0, NULL);
        return 0;
    }

    send_proxy_message(destination, message);
    return 1;
}

/* ---- setup ---- */

static int init_console(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int win_height;

    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hConsole == INVALID_HANDLE_VALUE) return 0;
    if (!GetConsoleScreenBufferInfo(g_hConsole, &csbi)) return 0;

    g_console_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    g_win_top = csbi.srWindow.Top;
    win_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    g_msg_top = g_win_top + 1;       /* row 0 is reserved for the system banner */
    g_msg_rows = win_height - 2;     /* banner row + input row */
    g_input_row = g_win_top + win_height - 1;

    SetConsoleTitleA("Hytera TMS - BBS Chat");

    {
        int row;
        for (row = 0; row < win_height; row++) {
            clear_row(g_win_top + row, COLOR_DEFAULT);
        }
    }

    draw_row_text(g_win_top, SYSTEM_BANNER, COLOR_LIGHTBLUE);

    return 1;
}

static int init_sockets(void) {
    WSADATA wsa;
    int reuse = 1;
    int broadcast_enable = 1;
    struct sockaddr_in devlog_local;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    /* devlog receive socket (10525) - same conventions as chat.c / the
       responder's proxy socket */
    g_devlog_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_devlog_sock == INVALID_SOCKET) return 0;
    setsockopt(g_devlog_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    setsockopt(g_devlog_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast_enable, sizeof(broadcast_enable));

    memset(&devlog_local, 0, sizeof(devlog_local));
    devlog_local.sin_family = AF_INET;
    devlog_local.sin_port = htons(DEVLOG_PORT);
    devlog_local.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_devlog_sock, (struct sockaddr *)&devlog_local, sizeof(devlog_local)) != 0) {
        fprintf(stderr, "devlog bind() failed: %d (is port %d already in use?)\n",
                WSAGetLastError(), DEVLOG_PORT);
        return 0;
    }

    /* proxy socket (10252) - used by send_proxy_message() to send, and
       now also bound so proxy_group_thread can receive group-message
       broadcasts on the same port */
    g_proxy_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_proxy_sock == INVALID_SOCKET) return 0;
    setsockopt(g_proxy_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    setsockopt(g_proxy_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast_enable, sizeof(broadcast_enable));

    {
        struct sockaddr_in proxy_local;
        memset(&proxy_local, 0, sizeof(proxy_local));
        proxy_local.sin_family = AF_INET;
        proxy_local.sin_port = htons(PROXY_PORT);
        proxy_local.sin_addr.s_addr = INADDR_ANY;
        if (bind(g_proxy_sock, (struct sockaddr *)&proxy_local, sizeof(proxy_local)) != 0) {
            fprintf(stderr, "proxy bind() failed: %d (is port %d already in use?)\n",
                    WSAGetLastError(), PROXY_PORT);
            return 0;
        }
    }

    memset(&g_proxy_target, 0, sizeof(g_proxy_target));
    g_proxy_target.sin_family = AF_INET;
    g_proxy_target.sin_port = htons(PROXY_PORT);
    g_proxy_target.sin_addr.s_addr = INADDR_BROADCAST;

    return 1;
}

int main(int argc, char *argv[]) {
    HANDLE hThread;
    HANDLE hGroupThread;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <RPT_ID>\n", argv[0]);
        fprintf(stderr, "example: %s 200\n", argv[0]);
        return 1;
    }
    g_rpt_id = (unsigned int)atoi(argv[1]);

    InitializeCriticalSection(&g_lock);

    if (!init_console()) {
        fprintf(stderr, "console init failed\n");
        return 1;
    }
    if (!init_sockets()) {
        fprintf(stderr, "socket init failed\n");
        return 1;
    }

    draw_input_prompt();

    /* _beginthreadex (not raw CreateThread) - this thread calls CRT
       functions (sscanf, _snprintf) which need a properly initialized
       per-thread CRT context; a thread started with plain CreateThread
       doesn't reliably get one on MinGW. Preventive fix matching the
       same class of bug found and fixed in HyteraDB.c's proxy thread. */
    hThread = (HANDLE)_beginthreadex(NULL, 0, devlog_thread, NULL, 0, NULL);
    if (!hThread) {
        fprintf(stderr, "failed to start devlog thread\n");
        return 1;
    }

    hGroupThread = (HANDLE)_beginthreadex(NULL, 0, proxy_group_thread, NULL, 0, NULL);
    if (!hGroupThread) {
        fprintf(stderr, "failed to start group message thread\n");
        return 1;
    }

    /* Main thread: raw keystroke capture for the input line. On Enter,
       the line is parsed as "{destination} {message}" and sent on the
       proxy port using RPT_ID as the source - unless it's "/exit"
       (either case), which quits the program locally without ever
       going out over UDP. */
    for (;;) {
        int ch = _getch();
        int send_now = 0;
        int want_exit = 0;
        char to_send[MAX_INPUT];

        if (ch == 0 || ch == 224) {
            _getch(); /* discard the second byte of an extended/arrow key */
            continue;
        }

        EnterCriticalSection(&g_lock);
        if (ch == '\r' || ch == '\n') {
            if (g_input_len > 0) {
                strncpy(to_send, g_input_buffer, sizeof(to_send) - 1);
                to_send[sizeof(to_send) - 1] = '\0';
                if (_stricmp(to_send, "/exit") == 0) {
                    want_exit = 1;
                } else {
                    send_now = 1;
                }
            }
            g_input_len = 0;
            g_input_buffer[0] = '\0';
        } else if (ch == 8) { /* backspace */
            if (g_input_len > 0) {
                g_input_len--;
                g_input_buffer[g_input_len] = '\0';
            }
        } else if (ch >= 32 && ch < 127 && g_input_len < MAX_INPUT - 1) {
            g_input_buffer[g_input_len++] = (char)ch;
            g_input_buffer[g_input_len] = '\0';
        }
        LeaveCriticalSection(&g_lock);

        if (want_exit) break;

        draw_input_prompt();

        if (send_now) {
            try_send_input(to_send);
        }
    }

    SetConsoleTextAttribute(g_hConsole, COLOR_DEFAULT);
    return 0;
}
