/*
 * HyteraDB.c
 *
 * The user's existing simple database program (DB5.c: New/Find/Seek/Show/
 * Delete/List against a flat text file), extended to also answer dot-
 * commands from radios over the LAN proxy used by hytera_tms_responder.c
 * and hytera_bbs_chat.c.
 *
 * Runs BOTH interfaces at once:
 *   - the original interactive "> " console prompt (unchanged behaviour)
 *   - a background thread that answers radio commands over UDP port 10252
 *
 * Build (MinGW / gcc on Windows 10):
 *      gcc HyteraDB.c -o HyteraDB.exe -lws2_32
 *
 * Run:
 *      HyteraDB.exe <RPT_ID>
 *      HyteraDB.exe 200
 *
 * --------------------------------------------------------------------
 * Radio-facing protocol (UDP port 10252, SO_REUSEADDR + SO_BROADCAST,
 * bound INADDR_ANY, broadcast 255.255.255.255 - same conventions as
 * chat.c / hytera_tms_responder.c / hytera_bbs_chat.c):
 *
 *   Incoming (from hytera_tms_responder.c's proxy, relaying a radio's
 *   message): "[SMS-TS1] {source} to {destination} > {message}"
 *   Only acted on when {destination} == RPT_ID AND {message} starts
 *   with '.' - anything else on this busy shared port is ignored.
 *   The leading '.' is stripped before the rest is handed to the same
 *   command parser the local console uses, e.g.:
 *       "[SMS-TS1] 203 to 200 > .find rob"  ->  "find rob"  ->  cmdFind("rob")
 *
 *   Outgoing (the DB's answer, wrapped back into the proxy protocol and
 *   broadcast so the responder picks it up and relays it out to the
 *   radio over TMS): "[SMS-TS1] {RPT_ID} to {source} > {answer}\r\n"
 *   e.g.: "[SMS-TS1] 200 to 203 > 1, 4 & 11"
 *
 *   If the command isn't recognized, NOTHING is sent back over UDP -
 *   other services on this same proxy also use a "." prefix, so an
 *   unrecognized dot-command might be meant for one of them, not us.
 *   "Unknown command" is still printed to HyteraDB's own console for
 *   visibility, it just never goes out on the wire.
 *
 *   The DB's Find/Seek output is a list of record numbers formatted as
 *   "1, 4 & 11" (comma-separated, "&" before the last one) rather than
 *   DB5.c's original bare "1,4,11", since that's the shape a human on a
 *   radio's small screen is meant to read. Any multi-line answer (e.g.
 *   List, if it were ever sent this way) has its line breaks collapsed
 *   to " | " before going out, since one UDP datagram = one line on
 *   every other program in this proxy chain.
 *
 *   "Exit" is intentionally NOT reachable from the radio side - it is
 *   only handled directly in the local console loop, exactly as in the
 *   original DB5.c, so nothing arriving over the air can shut the
 *   program down.
 * --------------------------------------------------------------------
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

#define DBFILE       "Database.txt"
#define MAXLINE      1024
#define MAXRECORDS   4096   /* cap on how many record numbers Find/Seek collect */

#define PROXY_PORT   10252

static CRITICAL_SECTION g_db_lock;
static unsigned int g_rpt_id;

static SOCKET g_proxy_sock;
static struct sockaddr_in g_proxy_target;

/* ---- small output-buffer helper, used instead of printf so command
   handlers can serve both the local console and the network reply from
   one code path ---- */

static void outapp(char *out, size_t outsize, const char *fmt, ...) {
    size_t len = strlen(out);
    va_list ap;
    if (len >= outsize) return;
    va_start(ap, fmt);
    _vsnprintf(out + len, outsize - len, fmt, ap);
    va_end(ap);
    out[outsize - 1] = '\0';
}

/* ---- original DB5.c helpers, unchanged ---- */

static int startsWithIgnoreCase(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text == '\0') return 0;
        if (tolower((unsigned char)*text) != tolower((unsigned char)*prefix)) return 0;
        text++;
        prefix++;
    }
    return 1;
}

static int containsIgnoreCase(const char *haystack, const char *needle) {
    char h[MAXLINE];
    char n[MAXLINE];
    strcpy(h, haystack);
    strcpy(n, needle);
    _strlwr(h);
    _strlwr(n);
    return strstr(h, n) != NULL;
}

static void createDatabase(void) {
    FILE *fp = fopen(DBFILE, "r");
    if (fp == NULL) {
        fp = fopen(DBFILE, "w");
        if (fp) fclose(fp);
    } else {
        fclose(fp);
    }
}

/* Formats a list of record numbers as "1, 4 & 11" (or "No matches found"
   for an empty list, or a bare number for a single match). */
static void format_record_list(const int *records, int count, char *out, size_t outsize) {
    int i;
    if (count == 0) {
        outapp(out, outsize, "No matches found");
        return;
    }
    for (i = 0; i < count - 1; i++) {
        outapp(out, outsize, i == 0 ? "%d" : ", %d", records[i]);
    }
    if (count == 1) {
        outapp(out, outsize, "%d", records[0]);
    } else {
        outapp(out, outsize, " & %d", records[count - 1]);
    }
}

/* ---- command handlers: same logic as DB5.c, writing into `out` instead
   of printf'ing directly, so the same code answers both the console and
   the radio ---- */

static void cmdNew(char *data, char *out, size_t outsize) {
    FILE *fp;
    char line[MAXLINE];
    int highest = 0;
    int rec;

    fp = fopen(DBFILE, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%d >", &rec) == 1) {
                if (rec > highest) highest = rec;
            }
        }
        fclose(fp);
    }

    fp = fopen(DBFILE, "a");
    if (fp) {
        fprintf(fp, "%d > %s\n", highest + 1, data);
        fclose(fp);
    }
    outapp(out, outsize, "Added record %d", highest + 1);
}

static void cmdFind(char *text, char *out, size_t outsize) {
    FILE *fp;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];
    int records[MAXRECORDS];
    int count = 0;

    fp = fopen(DBFILE, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%d > %[^\n]", &rec, data) == 2) {
                if (containsIgnoreCase(data, text) && count < MAXRECORDS) {
                    records[count++] = rec;
                }
            }
        }
        fclose(fp);
    }
    format_record_list(records, count, out, outsize);
}

static void cmdSeek(char *text, char *out, size_t outsize) {
    FILE *fp;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];
    int records[MAXRECORDS];
    int count = 0;

    fp = fopen(DBFILE, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%d > %[^\n]", &rec, data) == 2) {
                if (startsWithIgnoreCase(data, text) && count < MAXRECORDS) {
                    records[count++] = rec;
                }
            }
        }
        fclose(fp);
    }
    format_record_list(records, count, out, outsize);
}

static void cmdShow(int wanted, char *out, size_t outsize) {
    FILE *fp;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];

    fp = fopen(DBFILE, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%d > %[^\n]", &rec, data) == 2) {
                if (rec == wanted) {
                    outapp(out, outsize, "%d > %s", rec, data);
                    fclose(fp);
                    return;
                }
            }
        }
        fclose(fp);
    }
    outapp(out, outsize, "Record not found");
}

static void cmdDelete(int wanted, char *out, size_t outsize) {
    FILE *in;
    FILE *fout;
    char line[MAXLINE];
    int rec;
    char data[MAXLINE];
    int found = 0;

    in = fopen(DBFILE, "r");
    fout = fopen("Database.tmp", "w");
    if (in && fout) {
        while (fgets(line, sizeof(line), in)) {
            if (sscanf(line, "%d > %[^\n]", &rec, data) == 2) {
                if (rec == wanted) {
                    found = 1;
                    continue;
                }
                fprintf(fout, "%d > %s\n", rec, data);
            }
        }
    }
    if (in) fclose(in);
    if (fout) fclose(fout);
    remove(DBFILE);
    rename("Database.tmp", DBFILE);

    outapp(out, outsize, found ? "Deleted" : "Record not found");
}

static void cmdList(char *out, size_t outsize) {
    FILE *fp;
    char line[MAXLINE];

    fp = fopen(DBFILE, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            outapp(out, outsize, strlen(out) ? "\n%s" : "%s", line);
        }
        fclose(fp);
    }
    if (out[0] == '\0') outapp(out, outsize, "(empty)");
}

/* ---- shared command dispatch, used by both the console and the radio
   side. "Exit" is deliberately NOT handled here - see file header. ---- */

static void dispatch_command(const char *line, char *out, size_t outsize) {
    out[0] = '\0';

    EnterCriticalSection(&g_db_lock);
    if (_strnicmp(line, "New ", 4) == 0)
        cmdNew((char *)line + 4, out, outsize);
    else if (_strnicmp(line, "Find ", 5) == 0)
        cmdFind((char *)line + 5, out, outsize);
    else if (_strnicmp(line, "Seek ", 5) == 0)
        cmdSeek((char *)line + 5, out, outsize);
    else if (_strnicmp(line, "Show ", 5) == 0)
        cmdShow(atoi(line + 5), out, outsize);
    else if (_strnicmp(line, "Delete ", 7) == 0)
        cmdDelete(atoi(line + 7), out, outsize);
    else if (_stricmp(line, "List") == 0)
        cmdList(out, outsize);
    else
        outapp(out, outsize, "Unknown command");
    LeaveCriticalSection(&g_db_lock);
}

/* ---- radio-facing side (UDP port 10252) ---- */

static void send_proxy_reply(unsigned int destination, const char *message) {
    char line[MAXLINE + 64];
    int len = _snprintf(line, sizeof(line), "[SMS-TS1] %u to %u > %s\r\n",
                         g_rpt_id, destination, message);
    if (len < 0) len = (int)sizeof(line) - 1;
    sendto(g_proxy_sock, line, len, 0,
           (struct sockaddr *)&g_proxy_target, sizeof(g_proxy_target));
}

/* Collapses embedded CR/LF into " | " so a multi-line answer (e.g. from
   List) still fits the one-line-per-datagram convention every other
   program in this proxy chain relies on. Writes into a SEPARATE output
   buffer (never in-place) - collapsing a 1-byte break into a 3-byte
   " | " means an in-place version can have its write pointer overtake
   its own unread input, corrupting the very text it's still reading. */
static void collapse_to_single_line(const char *in, char *out, size_t outsize) {
    size_t oi = 0;
    int last_was_break = 0;
    const char *src = in;

    if (outsize == 0) return;

    while (*src && oi + 1 < outsize) {
        if (*src == '\r' || *src == '\n') {
            if (!last_was_break && oi != 0 && oi + 3 < outsize) {
                out[oi++] = ' ';
                out[oi++] = '|';
                out[oi++] = ' ';
            }
            last_was_break = 1;
        } else {
            out[oi++] = *src;
            last_was_break = 0;
        }
        src++;
    }
    out[oi] = '\0';
}

static void handle_proxy_line(char *line) {
    unsigned int source, destination;
    char msg[MAXLINE];
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }

    if (sscanf(line, "[SMS-TS1] %u to %u > %1023[^\r\n]", &source, &destination, msg) != 3) {
        return; /* not our protocol line, or malformed - ignore */
    }
    if (destination != g_rpt_id) {
        return; /* not addressed to us */
    }
    if (msg[0] != '.') {
        return; /* not a DB command */
    }

    {
        char answer[MAXLINE];
        char collapsed[MAXLINE];
        dispatch_command(msg + 1, answer, sizeof(answer));

        if (strcmp(answer, "Unknown command") == 0) {
            /* Other services on this proxy also use a "." command prefix,
               so an unrecognized dot-command from a radio isn't
               necessarily meant for us - stay silent on the wire, but
               still show it locally for visibility. */
            //printf("(radio %u sent unrecognized command \".%s\" - not replying)\n",
            //       source, msg + 1);
            return;
        }

        collapse_to_single_line(answer, collapsed, sizeof(collapsed));
        send_proxy_reply(source, collapsed);
    }
}

static unsigned __stdcall proxy_thread(void *unused) {
    unsigned char rbuf[1500];
    (void)unused;

    for (;;) {
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = recvfrom(g_proxy_sock, (char *)rbuf, sizeof(rbuf) - 1, 0,
                          (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            rbuf[n] = '\0';
            handle_proxy_line((char *)rbuf);
        }
    }
    return 0;
}

static int init_sockets(void) {
    WSADATA wsa;
    int reuse = 1;
    int broadcast_enable = 1;
    struct sockaddr_in local_addr;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    g_proxy_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_proxy_sock == INVALID_SOCKET) return 0;

    setsockopt(g_proxy_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    setsockopt(g_proxy_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast_enable, sizeof(broadcast_enable));

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(PROXY_PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_proxy_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
        fprintf(stderr, "bind() failed: %d (is port %d already in use?)\n",
                WSAGetLastError(), PROXY_PORT);
        return 0;
    }

    memset(&g_proxy_target, 0, sizeof(g_proxy_target));
    g_proxy_target.sin_family = AF_INET;
    g_proxy_target.sin_port = htons(PROXY_PORT);
    g_proxy_target.sin_addr.s_addr = INADDR_BROADCAST;

    return 1;
}

/* ---- local interactive console (same behaviour as DB5.c's main loop) ---- */

int main(int argc, char *argv[]) {
    char line[MAXLINE];
    uintptr_t hThread;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <RPT_ID>\n", argv[0]);
        fprintf(stderr, "example: %s 200\n", argv[0]);
        return 1;
    }
    g_rpt_id = (unsigned int)atoi(argv[1]);

    InitializeCriticalSection(&g_db_lock);
    createDatabase();

    if (!init_sockets()) {
        fprintf(stderr, "socket init failed\n");
        return 1;
    }
    /* _beginthreadex (not raw CreateThread) - this thread calls buffered
       CRT file I/O (fopen/fgets/fclose via cmdList/cmdNew/etc.), which
       needs a properly initialized per-thread CRT context. A thread
       started with plain CreateThread doesn't reliably get one on
       MinGW, which can crash unpredictably doing exactly this kind of
       work - suspected cause of the ".LIST"-from-radio crash. */
    hThread = _beginthreadex(NULL, 0, proxy_thread, NULL, 0, NULL);
    if (!hThread) {
        fprintf(stderr, "failed to start proxy thread\n");
        return 1;
    }

    printf("Simple Database (HyteraDB, RPT_ID=%u)\n", g_rpt_id);
    printf("Commands:\n");
    printf("  New text\n");
    printf("  Find text\n");
    printf("  Seek text\n");
    printf("  Show number\n");
    printf("  Delete number\n");
    printf("  List\n");
    printf("  Exit\n\n");
    printf("Radios can also send any of the above as a dot-command\n");
    printf("(e.g. \".find rob\") to RPT_ID %u via the UDP proxy.\n\n", g_rpt_id);

    while (1) {
        char answer[MAXLINE];

        printf("> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;

        if (_stricmp(line, "Exit") == 0) break;

        dispatch_command(line, answer, sizeof(answer));
        printf("%s\n", answer);
    }

    return 0;
}
