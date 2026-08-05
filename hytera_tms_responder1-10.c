/*
 * hytera_tms_responder.c
 *
 * Interfaces with a Hytera RD985 repeater's TMS (text messaging) service
 * on UDP port 30007. Sends the required wakeup/keepalive packets, listens
 * for incoming text messages, auto-replies to two commands the same way
 * the old FoxPro program did (".PING"->"PONG!", ".TIME"->date/time), and
 * bridges everything to a LAN-visible UDP proxy on port 10252 so other
 * software can send/receive radio text messages through this program.
 *
 * Build (MinGW / gcc on Windows 10):
 *      gcc hytera_tms_responder.c -o hytera_tms_responder.exe -lws2_32
 *
 * Run:
 *      hytera_tms_responder.exe <repeater_ip>
 *      hytera_tms_responder.exe 192.168.1.167
 *
 * --------------------------------------------------------------------
 * Frame formats implemented here (reverse-engineered from real captures
 * "Hytera_WORKING_TMS_PingPong.pcapng" and "Auto_Works_But_UDP_Chat_
 * Failed.pcapng" plus earlier sessions):
 *
 * 1) Control frames (6 bytes, no payload):
 *      wakeup     32 42 00 05 00 00   (send once at startup)
 *      keepalive  32 42 00 02 00 00   (send every 5 seconds)
 *      ack        32 42 00 01 <msg_id, 2 bytes BE>  (repeater -> us,
 *                 acknowledging a message we sent with that msg_id)
 *
 * 2) Received text / status frames (32-byte header + payload), sent
 *    FROM the repeater TO us:
 *      offset  size  field
 *      0x00    2     signature 0x3242
 *      0x02    2     constant 0x0020 (fixed header-size marker, NOT a
 *                    computed length - confirmed constant across every
 *                    sample regardless of total frame size)
 *      0x04    2     msg_id (repeater's own per-packet counter, BE -
 *                    increments on EVERY packet including retransmits,
 *                    so it is NOT useful for de-duplication)
 *      0x06    2     msg_type 0x8304 (constant, text/status frame class)
 *      0x08    4     source_id (observed constant 0x000000C8 = 200)
 *      0x0C    3     flags, constant 04 01 01
 *      0x0F    1     subtype 0x09
 *      0x10    1     addressing type - 0x80 = private message,
 *                    0x00 = group message (per Rob's notes, confirmed)
 *      0x11    1     constant 0xA1 on text frames, 0xA2 on delivery-
 *                    status reports
 *      0x12    2     remaining_len = 12 + text_byte_len (BE)
 *      0x14    4     seq (BE)
 *      0x17    1     **SMS number** (0-255, per Rob's notes) - identifies
 *                    one logical message. On TEXT frames: the repeater
 *                    RESENDS the same text a few times with this byte
 *                    unchanged while waiting for a reply it never gets,
 *                    so we de-dupe incoming text on this byte. On STATUS
 *                    frames: this is the low byte of the msg_id WE used
 *                    when we sent the message this status refers to, so
 *                    we match status reports back to our pending sends
 *                    on this byte.
 *      0x18    4     dest_value (BE) - us (200) on messages addressed
 *                    to our station
 *      0x1C    4     **sender_id** (BE) - the originating radio's ID on
 *                    text frames (confirmed via Rob's notes cross-checked
 *                    against real capture bytes - not a constant as
 *                    earlier sessions assumed)
 *      0x20    1     on STATUS frames: delivery status. 0x00 = delivered,
 *                    0x08 = NOT delivered (confirmed from a real capture
 *                    where two unsolicited proxy-originated sends both
 *                    got 0x08 while immediate auto-replies to an active
 *                    radio got 0x00 - consistent with the repeater being
 *                    unable to page a radio that isn't currently active/
 *                    listening; keying the radio first reportedly makes
 *                    delivery reliable)
 *      0x20    ...   on TEXT frames: text, UTF-16LE
 *      ...     1     checksum (algorithm not solved - not validated here)
 *      last    1     end marker 0x03
 *
 * 3) Outgoing private-send frames, sent FROM us TO the repeater
 *    (confirmed working shape, reproduced from the real captured
 *    "PONG!" and time-string replies):
 *      offset  size  field
 *      0x00    2     signature 0x3242
 *      0x02    2     constant 0x0000
 *      0x04    2     msg_id (our own counter, BE, starts at 1)
 *      0x06    1     subtype 0x09
 *      0x07    2     routing_flags 0x00A1 (BE)
 *      0x09    2     remaining_len = 12 + text_byte_len (BE)
 *      0x0B    4     seq (BE) - observed non-zero in real captures;
 *                    exact required value unconfirmed, 0 also works
 *      0x0F    1     addr_marker 0x0A
 *      0x10    1     pad 0x00
 *      0x11    2     dest_radio_id (BE)
 *      0x13    4     dest_ip-shaped field - captured value 0A 00 00 C8;
 *                    confirmed NOT required to be a real IP for delivery
 *      0x17    ...   text, UTF-16LE
 *      ...     1     checksum - captured working value is 0x00
 *      last    1     end marker 0x03
 *
 * 4) LAN proxy (UDP port 10252, one line per datagram, CRLF-terminated):
 *      radio -> LAN:      [SMS-TS1] {sender} to {dest} > {message}
 *      LAN -> radio:      [SMS-TS1] {source} to {dest} > {message}
 *      delivery delayed:  [SMS-TS1] {dest} has not yet received your
 *                          message, Please wait.
 *      delivery gave up:  [SMS-TS1] FAILED TO DELIVER {sender} to {dest}
 *                          > {message}
 *    Socket uses SO_REUSEADDR + SO_BROADCAST and broadcasts to
 *    255.255.255.255, matching the conventions of the user's own
 *    reference chat program (github.com/2E0RPT/Hytera/blob/main/chat.c).
 *
 * 5) Retry policy for outgoing private sends (mirrors calling a VHF/UHF
 *    mobile radio on the edge of range - a handful of quick retries with
 *    a couple of longer, more patient waits mixed in):
 *      after fail  1: wait  5s, retry
 *      after fail  2: wait  2s, retry
 *      after fail  3: wait 10s, retry, and send a "please wait" proxy
 *                     notice
 *      after fail  4: wait  2s, retry
 *      after fail  5: wait 15s, retry
 *      after fail 6-9: retry instantly (no wait)
 *      after fail 10: give up, send a "FAILED TO DELIVER" proxy notice
 *    A pending send is matched to the repeater's delivery-status report
 *    via the SMS-number byte described above. If no status report shows
 *    up at all within STATUS_TIMEOUT_SECONDS, that's treated the same as
 *    an explicit failure so a lost/missing status report can't wedge a
 *    message forever.
 *
 * 6) Dev-log broadcast (UDP port 10525, one line per datagram, CRLF-
 *    terminated) - a read-only event feed for other Hytera programs the
 *    user is developing, separate from the port 10252 proxy above.
 *    Socket setup matches port 10252 / chat.c exactly (SO_REUSEADDR +
 *    SO_BROADCAST, bound INADDR_ANY, broadcast to 255.255.255.255).
 *    SMS={sms_number} identifies one logical message and stays the same
 *    across all of its TX/TRY/PASS/FAIL events even if a message is
 *    retried under a new internal msg_id (the FIRST attempt's SMS number
 *    is used throughout, so listeners can correlate the whole lifecycle
 *    of one message via a single number):
 *      message arrives from a radio:   [SMS-TS1] RX > {sender} > {dest} > SMS={n} > {message}
 *      message arrives from the proxy: [SMS-TS1] TX > PROXY > SMS={n} > {message}
 *      message is retried after a fail:[SMS-TS1] TRY > PROXY > SMS={n} > {message}
 *      message confirmed delivered:    [SMS-TS1] PASS > PROXY > SMS={n} > {message}
 *      message failed on 10th attempt: [SMS-TS1] FAIL > PROXY > SMS={n} Failed or unconfirmed.
 *    {dest} on RX is the REAL destination from the frame, not necessarily
 *    OUR_STATION_ID - the repeater relays private messages between OTHER
 *    radios to us too (we're effectively monitoring, not always the
 *    recipient), so this can be a different radio's ID entirely.
 *    NOTE: the user's spec used the literal word "PROXY" for TX/TRY/PASS/
 *    FAIL without a variable, so every queued private send (both proxy-
 *    forwarded messages AND the .PING/.TIME auto-replies) is logged this
 *    way - not just ones that came in via port 10252. Worth flagging in
 *    case auto-replies should be excluded/labelled differently instead.
 * --------------------------------------------------------------------
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#define TMS_PORT            30007
#define KEEPALIVE_SECONDS   5

/* LAN proxy: other software (same PC or LAN) can send/receive plain-text
   SMS-TS1 lines on this UDP port to talk to radios through us. */
#define PROXY_PORT          10252

/* Dev-log broadcast: a read-only event feed (RX/TX/TRY/PASS/FAIL) for
   other Hytera programs the user is developing. Same socket conventions
   as PROXY_PORT / chat.c, just a separate port and one-way. */
#define DEVLOG_PORT          10525

/* Our own station/repeater ID, as seen in the confirmed TX frame's
   sender-ID bytes (0x0A 00 00 C8 = marker + station ID 200). Used as the
   "source" in outgoing proxy lines and validated as the expected
   "destination" in incoming proxy lines. */
#define OUR_STATION_ID       200

/* How many recent SMS numbers to remember for de-duplication of INCOMING
   text. The repeater resends the same text a few times with the same SMS
   number while waiting for a reply it never gets - we only want to react
   once. */
#define SMS_HISTORY_SIZE    16

/* Retry/backoff for OUTGOING private sends. Index = fail_count (1..9);
   index 0 is unused. After fail_count reaches 10 we give up. Fails 6-9
   retry instantly (0s) - by that point we've already waited through the
   patient early attempts, so keep hammering quickly. */
static const int RETRY_WAIT_SECONDS[10] = {0, 5, 2, 10, 2, 15, 0, 0, 0, 0};
#define MAX_FAILS_BEFORE_GIVEUP   10
#define NOTICE_AFTER_FAIL_COUNT    3

/* If the repeater never sends a delivery-status report at all for an
   attempt (lost packet, etc.), treat it as failed after this many
   seconds so it doesn't wedge forever. Real captures show status
   reports arriving ~4-4.5s after the send, so this leaves margin. */
#define STATUS_TIMEOUT_SECONDS     8

#define MAX_PENDING_SENDS   16

static const unsigned char WAKEUP_PKT[6]    = {0x32,0x42,0x00,0x05,0x00,0x00};
static const unsigned char KEEPALIVE_PKT[6] = {0x32,0x42,0x00,0x02,0x00,0x00};

static SOCKET g_sock;
static struct sockaddr_in g_repeater_addr;
static unsigned short g_next_msg_id = 1;

static SOCKET g_proxy_sock;
static struct sockaddr_in g_proxy_broadcast_addr;

/* Bodies (without "[SMS-TS1] " prefix or CRLF) we recently broadcast
   ourselves on the proxy port, so handle_proxy_line() can recognize our
   own broadcast coming back (normal for UDP broadcast on the same box)
   and not re-process it as a new command. This matters most for group
   messages: a group RX-relay is addressed "to TG{n}", which looks
   structurally identical to a legitimate outgoing group-send command,
   so (unlike private messages, which can be told apart by "addressed
   to us") there's no way to distinguish the two just from the message
   shape - without this, a received group message gets relayed, heard
   back, and re-sent as a "new" send, which gets received and relayed
   again, forever. */
#define RECENT_SENT_HISTORY 8
static char g_recent_sent[RECENT_SENT_HISTORY][300];
static int g_recent_sent_count = 0;
static int g_recent_sent_next = 0;

static void recent_sent_remember(const char *body) {
    strncpy(g_recent_sent[g_recent_sent_next], body, sizeof(g_recent_sent[0]) - 1);
    g_recent_sent[g_recent_sent_next][sizeof(g_recent_sent[0]) - 1] = '\0';
    g_recent_sent_next = (g_recent_sent_next + 1) % RECENT_SENT_HISTORY;
    if (g_recent_sent_count < RECENT_SENT_HISTORY) g_recent_sent_count++;
}

static int recent_sent_contains(const char *body) {
    int i;
    for (i = 0; i < RECENT_SENT_HISTORY && i < g_recent_sent_count; i++) {
        if (strcmp(g_recent_sent[i], body) == 0) return 1;
    }
    return 0;
}

static SOCKET g_devlog_sock;
static struct sockaddr_in g_devlog_broadcast_addr;

/* Recently-handled SMS numbers (incoming text de-dup), oldest overwritten first. */
static int g_sms_history[SMS_HISTORY_SIZE];
static int g_sms_history_count = 0;
static int g_sms_history_next = 0;

static int sms_already_handled(unsigned char sms_number) {
    int i;
    for (i = 0; i < g_sms_history_count; i++) {
        if (g_sms_history[i] == sms_number) return 1;
    }
    return 0;
}

static void sms_mark_handled(unsigned char sms_number) {
    g_sms_history[g_sms_history_next] = sms_number;
    g_sms_history_next = (g_sms_history_next + 1) % SMS_HISTORY_SIZE;
    if (g_sms_history_count < SMS_HISTORY_SIZE) g_sms_history_count++;
}

/* One outgoing private send being tracked through to delivery or give-up. */
typedef struct {
    int in_use;
    unsigned int dest_radio_id;
    unsigned int sender_id;      /* "source" to quote in retry/failure notices */
    char text[256];
    int fail_count;
    unsigned short last_msg_id;  /* msg_id of the most recent attempt */
    unsigned char origin_sms_number; /* low byte of the FIRST attempt's msg_id -
                                         stays fixed across retries, used to
                                         correlate TX/TRY/PASS/FAIL on the devlog port */
    time_t sent_at;              /* when the most recent attempt was sent */
    time_t next_retry_time;      /* 0 = not scheduled (waiting on a status report) */
    int notice_sent;
} pending_send_t;

static pending_send_t g_pending[MAX_PENDING_SENDS];

/* ---- little helpers for big-endian field packing ---- */

static void put_u16be(unsigned char *buf, unsigned short v) {
    buf[0] = (unsigned char)(v >> 8);
    buf[1] = (unsigned char)(v & 0xFF);
}

static void put_u32be(unsigned char *buf, unsigned int v) {
    buf[0] = (unsigned char)(v >> 24);
    buf[1] = (unsigned char)(v >> 16);
    buf[2] = (unsigned char)(v >> 8);
    buf[3] = (unsigned char)(v & 0xFF);
}

static unsigned short get_u16be(const unsigned char *buf) {
    return (unsigned short)((buf[0] << 8) | buf[1]);
}

static unsigned int get_u32be(const unsigned char *buf) {
    return ((unsigned int)buf[0] << 24) | ((unsigned int)buf[1] << 16) |
           ((unsigned int)buf[2] << 8) | (unsigned int)buf[3];
}

/* ---- low-level TMS sending ---- */

static void send_raw(const unsigned char *buf, int len) {
    sendto(g_sock, (const char *)buf, len, 0,
           (struct sockaddr *)&g_repeater_addr, sizeof(g_repeater_addr));
}

static void send_wakeup(void) {
    send_raw(WAKEUP_PKT, sizeof(WAKEUP_PKT));
    printf("-> wakeup\n");
}

static void send_keepalive(void) {
    send_raw(KEEPALIVE_PKT, sizeof(KEEPALIVE_PKT));
    /* printf("-> keepalive\n"); */
}

/*
 * Builds and sends ONE attempt of a private-send text frame to
 * dest_radio_id, following the exact shape confirmed from the real
 * "PONG!"/time-reply capture, including checksum=0x00 and the trailing
 * extra 0x00 byte the legacy software sent. Returns the msg_id used for
 * this attempt, so the caller can match a later delivery-status report
 * back to it.
 */
static unsigned short send_private_text_raw(unsigned short dest_radio_id, const char *ascii_text) {
    unsigned char buf[512];
    int text_chars = (int)strlen(ascii_text);
    int text_bytes = text_chars * 2;          /* UTF-16LE */
    unsigned short remaining_len = (unsigned short)(12 + text_bytes);
    unsigned short msg_id = g_next_msg_id++;
    int i;

    unsigned char *p = buf;

    put_u16be(p, 0x3242); p += 2;              /* signature */
    put_u16be(p, 0x0000); p += 2;              /* constant */
    put_u16be(p, msg_id); p += 2;              /* our msg_id */
    *p++ = 0x09;                               /* subtype */
    put_u16be(p, 0x00A1); p += 2;              /* routing_flags */
    put_u16be(p, remaining_len); p += 2;       /* remaining_len */
    put_u32be(p, 0x30000000u | msg_id); p += 4;/* seq (mirrors legacy capture pattern) */
    *p++ = 0x0A;                               /* addr_marker */
    *p++ = 0x00;                               /* pad */
    put_u16be(p, dest_radio_id); p += 2;       /* dest_radio_id */
    *p++ = 0x0A; *p++ = 0x00; *p++ = 0x00; *p++ = 0xC8; /* dest_ip-shaped field (captured value) */

    /* text, UTF-16LE (ASCII-safe subset only, which covers PONG!/time strings) */
    for (i = 0; i < text_chars; i++) {
        *p++ = (unsigned char)ascii_text[i];
        *p++ = 0x00;
    }

    *p++ = 0x00;    /* checksum - 0x00 matches the real working capture */
    *p++ = 0x03;    /* end marker */
    *p++ = 0x00;    /* trailing byte present in the legacy capture; harmless */

    send_raw(buf, (int)(p - buf));
    printf("-> private send to radio %u: \"%s\" (msg_id=%u)\n",
           dest_radio_id, ascii_text, msg_id);
    return msg_id;
}

/*
 * EXPERIMENTAL / UNCONFIRMED: builds and sends a group-call text frame,
 * by analogy with the confirmed private-send frame above. Nothing in
 * any capture so far shows a WORKING outgoing group send - this is
 * inferred from the received-frame evidence only (see the group-message
 * notes near handle_group_text_message() below), specifically that real
 * inbound group texts use routing 0x00B1 (vs 0x00A1 for private) with
 * the talkgroup number in the same slot the private frame uses for a
 * radio ID. The only change from send_private_text_raw() is the routing
 * byte and treating the "dest" field as a talkgroup number - everything
 * else (checksum=0x00, trailing 0x00, etc.) is carried over unchanged.
 * Needs a real-world test to confirm the repeater actually accepts it.
 */
static unsigned short send_group_text_raw(unsigned int talkgroup, const char *ascii_text) {
    unsigned char buf[512];
    int text_chars = (int)strlen(ascii_text);
    int text_bytes = text_chars * 2;          /* UTF-16LE */
    unsigned short remaining_len = (unsigned short)(12 + text_bytes);
    unsigned short msg_id = g_next_msg_id++;
    int i;

    unsigned char *p = buf;

    put_u16be(p, 0x3242); p += 2;              /* signature */
    put_u16be(p, 0x0000); p += 2;              /* constant */
    put_u16be(p, msg_id); p += 2;              /* our msg_id */
    *p++ = 0x09;                               /* subtype */
    put_u16be(p, 0x00B1); p += 2;              /* routing_flags - GROUP, not 0x00A1 */
    put_u16be(p, remaining_len); p += 2;       /* remaining_len */
    put_u32be(p, 0x30000000u | msg_id); p += 4;/* seq (mirrors legacy capture pattern) */
    *p++ = 0x0A;                               /* addr_marker */
    *p++ = 0x00;                               /* pad */
    put_u16be(p, (unsigned short)talkgroup); p += 2; /* talkgroup number, in the dest_radio_id slot */
    *p++ = 0x0A; *p++ = 0x00; *p++ = 0x00; *p++ = 0xC8; /* dest_ip-shaped field (captured value) */

    for (i = 0; i < text_chars; i++) {
        *p++ = (unsigned char)ascii_text[i];
        *p++ = 0x00;
    }

    *p++ = 0x00;    /* checksum - 0x00 matches the confirmed private-send capture */
    *p++ = 0x03;    /* end marker */
    *p++ = 0x00;    /* trailing byte present in the legacy capture; harmless */

    send_raw(buf, (int)(p - buf));
    printf("-> [EXPERIMENTAL] group send to TG%u: \"%s\" (msg_id=%u)\n",
           talkgroup, ascii_text, msg_id);
    return msg_id;
}

/* ---- LAN proxy (port 10252): "[SMS-TS1] ...\r\n" ---- */

/* Sends a raw already-formatted body as "[SMS-TS1] {body}\r\n". */
static void proxy_send_raw(const char *body) {
    char line[512];
    int len = _snprintf(line, sizeof(line), "[SMS-TS1] %s\r\n", body);
    if (len < 0) len = (int)sizeof(line) - 1;
    recent_sent_remember(body);
    sendto(g_proxy_sock, line, len, 0,
           (struct sockaddr *)&g_proxy_broadcast_addr, sizeof(g_proxy_broadcast_addr));
    printf("-> proxy: %s\n", body);
}

/* Sends "[SMS-TS1] {source} to {dest} > {message}\r\n" - the normal
   radio<->LAN message format. */
static void proxy_announce(unsigned int source_id, unsigned int dest_id, const char *message) {
    char body[480];
    _snprintf(body, sizeof(body), "%u to %u > %s", source_id, dest_id, message);
    proxy_send_raw(body);
}

/* Sends "[SMS-TS1] {source} to TG{talkgroup} > {message}\r\n" - the
   group-message equivalent of proxy_announce(), using the "TG" prefix
   to distinguish a talkgroup number from an individual radio ID. */
static void proxy_announce_group(unsigned int source_id, unsigned int talkgroup, const char *message) {
    char body[480];
    _snprintf(body, sizeof(body), "%u to TG%u > %s", source_id, talkgroup, message);
    proxy_send_raw(body);
}

/* ---- Dev-log broadcast (port 10525): "[SMS-TS1] <EVENT> > ... > SMS={n} > ...\r\n" ---- */

static void devlog_send_raw(const char *body) {
    char line[512];
    int len = _snprintf(line, sizeof(line), "[SMS-TS1] %s\r\n", body);
    if (len < 0) len = (int)sizeof(line) - 1;
    sendto(g_devlog_sock, line, len, 0,
           (struct sockaddr *)&g_devlog_broadcast_addr, sizeof(g_devlog_broadcast_addr));
    printf("-> devlog: %s\n", body);
}

static void devlog_rx(unsigned int sender_id, unsigned int dest_id, unsigned char sms_number, const char *message) {
    char body[480];
    _snprintf(body, sizeof(body), "RX > %u > %u > SMS=%u > %s", sender_id, dest_id, sms_number, message);
    devlog_send_raw(body);
}

static void devlog_tx(unsigned char sms_number, const char *message) {
    char body[480];
    _snprintf(body, sizeof(body), "TX > PROXY > SMS=%u > %s", sms_number, message);
    devlog_send_raw(body);
}

static void devlog_try(unsigned char sms_number, const char *message) {
    char body[480];
    _snprintf(body, sizeof(body), "TRY > PROXY > SMS=%u > %s", sms_number, message);
    devlog_send_raw(body);
}

static void devlog_pass(unsigned char sms_number, const char *message) {
    char body[480];
    _snprintf(body, sizeof(body), "PASS > PROXY > SMS=%u > %s", sms_number, message);
    devlog_send_raw(body);
}

static void devlog_fail(unsigned char sms_number) {
    char body[100];
    _snprintf(body, sizeof(body), "FAIL > PROXY > SMS=%u Failed or unconfirmed.", sms_number);
    devlog_send_raw(body);
}

/* ---- pending-send tracking / retry logic ---- */

static pending_send_t *pending_find_free_slot(void) {
    int i;
    for (i = 0; i < MAX_PENDING_SENDS; i++) {
        if (!g_pending[i].in_use) return &g_pending[i];
    }
    return NULL;
}

static pending_send_t *pending_find_by_msg_id(unsigned char sms_number) {
    int i;
    for (i = 0; i < MAX_PENDING_SENDS; i++) {
        if (g_pending[i].in_use &&
            (unsigned char)(g_pending[i].last_msg_id & 0xFF) == sms_number) {
            return &g_pending[i];
        }
    }
    return NULL;
}

/*
 * Registers a new logical message to send and performs its first
 * attempt. Called both for auto-replies (.PING/.TIME) and for messages
 * coming in from the LAN proxy.
 */
static void queue_private_send(unsigned int sender_id, unsigned int dest_id, const char *text) {
    pending_send_t *p = pending_find_free_slot();
    if (!p) {
        printf("   (too many messages in flight, dropping send to %u)\n", dest_id);
        return;
    }
    p->in_use = 1;
    p->dest_radio_id = dest_id;
    p->sender_id = sender_id;
    strncpy(p->text, text, sizeof(p->text) - 1);
    p->text[sizeof(p->text) - 1] = '\0';
    p->fail_count = 0;
    p->notice_sent = 0;
    p->next_retry_time = 0;
    p->last_msg_id = send_private_text_raw((unsigned short)dest_id, p->text);
    p->origin_sms_number = (unsigned char)(p->last_msg_id & 0xFF);
    p->sent_at = time(NULL);
    devlog_tx(p->origin_sms_number, p->text);
}

/* Records one failed attempt for a pending send and schedules the next
   retry (or gives up), per the retry policy documented at the top of
   this file. */
static void pending_record_failure(pending_send_t *p) {
    p->fail_count++;
    printf("   delivery attempt %d failed for radio %u: \"%s\"\n",
           p->fail_count, p->dest_radio_id, p->text);

    if (p->fail_count >= MAX_FAILS_BEFORE_GIVEUP) {
        char body[480];
        printf("   giving up on radio %u after %d failed attempts\n",
               p->dest_radio_id, p->fail_count);
        _snprintf(body, sizeof(body), "FAILED TO DELIVER %u to %u > %s",
                  p->sender_id, p->dest_radio_id, p->text);
        proxy_send_raw(body);
        devlog_fail(p->origin_sms_number);
        p->in_use = 0;
        return;
    }

    if (p->fail_count == NOTICE_AFTER_FAIL_COUNT && !p->notice_sent) {
        char body[300];
        _snprintf(body, sizeof(body), "%u has not yet received your message, Please wait.",
                  p->dest_radio_id);
        proxy_send_raw(body);
        p->notice_sent = 1;
    }

    devlog_try(p->origin_sms_number, p->text);

    if (RETRY_WAIT_SECONDS[p->fail_count] == 0) {
        /* instant retry - don't wait for the next ~1s poll tick */
        p->next_retry_time = 0;
        p->last_msg_id = send_private_text_raw((unsigned short)p->dest_radio_id, p->text);
        p->sent_at = time(NULL);
    } else {
        p->next_retry_time = time(NULL) + RETRY_WAIT_SECONDS[p->fail_count];
    }
}

/* Called when a delivery-status frame arrives from the repeater. */
static void handle_delivery_status(unsigned char sms_number, unsigned char status) {
    pending_send_t *p = pending_find_by_msg_id(sms_number);
    if (!p) {
        return; /* status for something we're not tracking (or already resolved) */
    }
    if (status == 0x00) {
        printf("   delivered to radio %u: \"%s\"\n", p->dest_radio_id, p->text);
        devlog_pass(p->origin_sms_number, p->text);
        p->in_use = 0;
        return;
    }
    pending_record_failure(p);
}

/* Called roughly once a second from the main loop: fires any due
   retries, and treats a pending send with no status report at all
   within STATUS_TIMEOUT_SECONDS as a failure so it can't wedge. */
static void process_retries(void) {
    int i;
    time_t now = time(NULL);
    for (i = 0; i < MAX_PENDING_SENDS; i++) {
        pending_send_t *p = &g_pending[i];
        if (!p->in_use) continue;

        if (p->next_retry_time != 0 && now >= p->next_retry_time) {
            p->next_retry_time = 0;
            p->last_msg_id = send_private_text_raw((unsigned short)p->dest_radio_id, p->text);
            p->sent_at = now;
            continue;
        }

        if (p->next_retry_time == 0 && (now - p->sent_at) >= STATUS_TIMEOUT_SECONDS) {
            printf("   (no delivery-status report for radio %u within %ds, treating as failed)\n",
                   p->dest_radio_id, STATUS_TIMEOUT_SECONDS);
            pending_record_failure(p);
        }
    }
}

/*
 * Parses one incoming proxy line of the form:
 *   [SMS-TS1] {source} to {destination} > {message}
 * and, if the destination is a radio (not us), queues it for sending
 * (with retry) over TMS. Malformed or unrecognized lines are ignored.
 *
 * A destination written as "TG{n}" (e.g. "TG1") is a talkgroup send
 * instead - see send_group_text_raw()'s comments for why this path is
 * EXPERIMENTAL and unconfirmed. Group sends are fired once, with no
 * retry/backoff and no delivery-status tracking - it isn't clear a
 * per-recipient "delivered" status even makes sense for a group call,
 * and retrying could mean re-broadcasting the same text to an entire
 * talkgroup unnecessarily.
 */
static void handle_proxy_line(char *line) {
    unsigned int source_id, dest_id, talkgroup;
    char *arrow;
    char *msg;
    size_t len;

    /* strip trailing CR/LF */
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }

    if (strncmp(line, "[SMS-TS1] ", 10) != 0) {
        printf("   (proxy: ignoring line without [SMS-TS1] prefix)\n");
        return;
    }

    if (recent_sent_contains(line + 10)) {
        printf("   (proxy: ignoring our own recent broadcast - self-echo)\n");
        return;
    }

    arrow = strstr(line, " > ");
    if (!arrow) {
        printf("   (proxy: missing ' > ' message separator)\n");
        return;
    }
    msg = arrow + 3;

    if (sscanf(line + 10, "%u to TG%u", &source_id, &talkgroup) == 2) {
        printf("<- proxy: %u to TG%u > %s\n", source_id, talkgroup, msg);
        send_group_text_raw(talkgroup, msg);
        return;
    }

    if (sscanf(line + 10, "%u to %u", &source_id, &dest_id) != 2) {
        printf("   (proxy: could not parse source/destination from line)\n");
        return;
    }

    printf("<- proxy: %u to %u > %s\n", source_id, dest_id, msg);

    if (dest_id == OUR_STATION_ID) {
        printf("   (proxy: message addressed to us, not a radio - ignoring)\n");
        return;
    }
    if (dest_id == 0 || dest_id > 0xFFFF) {
        printf("   (proxy: destination %u out of range for a radio ID - ignoring)\n", dest_id);
        return;
    }

    queue_private_send(source_id, dest_id, msg);
}

/* ---- receiving / dispatch ---- */

static void handle_text_message(const unsigned char *buf, int len) {
    /* text runs from offset 0x20 to len-2 (checksum + end marker) */
    int text_start = 0x20;
    int text_end = len - 2; /* exclusive */
    int text_bytes = text_end - text_start;
    int text_chars = text_bytes / 2;
    char text[256];
    int i;
    unsigned char sms_number;
    unsigned int sender_id;
    unsigned int dest_id;

    if (len < 0x20) {
        printf("   (frame too short to contain header fields, skipping)\n");
        return;
    }
    if (text_bytes <= 0 || text_chars >= (int)sizeof(text)) {
        printf("   (text message with unexpected length, skipping)\n");
        return;
    }

    sms_number = buf[0x17];
    dest_id = get_u32be(buf + 0x18);
    sender_id = get_u32be(buf + 0x1C);

    for (i = 0; i < text_chars; i++) {
        unsigned char lo = buf[text_start + i * 2];
        /* ASCII-range UTF-16LE only, good enough for command text */
        text[i] = (char)lo;
    }
    text[text_chars] = '\0';

    printf("   text: \"%s\" (sms_number=%u, sender_id=%u, dest_id=%u)\n",
           text, sms_number, sender_id, dest_id);

    if (sms_already_handled(sms_number)) {
        printf("   (duplicate/retransmit of sms_number=%u, already replied - skipping)\n",
               sms_number);
        return;
    }
    sms_mark_handled(sms_number);

    /* dest_id is the ACTUAL destination from the frame - the repeater
       relays private messages between OTHER radios to us too (we're
       effectively monitoring), not just ones addressed to our own
       station, so this must never be assumed to be OUR_STATION_ID. */
    proxy_announce(sender_id, dest_id, text);
    devlog_rx(sender_id, dest_id, sms_number, text);

    if (dest_id != OUR_STATION_ID) {
        /* not addressed to us - relay/log only, no auto-reply */
        return;
    }

    if (_stricmp(text, ".PING") == 0) {
        queue_private_send(OUR_STATION_ID, sender_id, "PONG!");
    } else if (_stricmp(text, ".TIME") == 0) {
        char timestr[64];
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %I:%M:%S %p", lt);
        queue_private_send(OUR_STATION_ID, sender_id, timestr);
    }
    /* unrecognized text: ignore, same as the legacy program */
}

/*
 * Handles a received GROUP (talkgroup) text message - routing 0x00B1,
 * confirmed from a real capture (Hytera_TMS_Group_Text.pcapng) showing
 * two genuine group sends with this routing value and dest_value=1 and
 * dest_value=2 respectively (the plain talkgroup number, not a special
 * constant). That capture also contained several OTHER messages with
 * routing 0x80A1 and dest_value=0x000004C8 (1224) whose text happened to
 * read "This is group 1" - those are NOT actually group messages by
 * Rob's addressing-type rule (0x80=private, 0x00=group - confirmed
 * earlier in this project); routing 0x80A1 there means that was really
 * a PRIVATE message whose text just mentioned "group 1". Only the
 * 0x00B1 pattern is treated as a real group message here.
 *
 * Same 32-byte header shape as handle_text_message() otherwise - only
 * the routing value and how dest_value is interpreted differ.
 */
static void handle_group_text_message(const unsigned char *buf, int len) {
    int text_start = 0x20;
    int text_end = len - 2;
    int text_bytes = text_end - text_start;
    int text_chars = text_bytes / 2;
    char text[256];
    int i;
    unsigned char sms_number;
    unsigned int sender_id;
    unsigned int talkgroup;

    if (len < 0x20) {
        printf("   (frame too short to contain header fields, skipping)\n");
        return;
    }
    if (text_bytes <= 0 || text_chars >= (int)sizeof(text)) {
        printf("   (group text message with unexpected length, skipping)\n");
        return;
    }

    sms_number = buf[0x17];
    talkgroup = get_u32be(buf + 0x18);
    sender_id = get_u32be(buf + 0x1C);

    for (i = 0; i < text_chars; i++) {
        unsigned char lo = buf[text_start + i * 2];
        text[i] = (char)lo;
    }
    text[text_chars] = '\0';

    printf("   group text: \"%s\" (TG%u, sms_number=%u, sender_id=%u)\n",
           text, talkgroup, sms_number, sender_id);

    /* Shares the same de-dup history as private messages/SMS numbers -
       if that ever turns out to cause cross-class false positives, give
       group messages their own history array. Not expected to matter
       in practice. */
    if (sms_already_handled(sms_number)) {
        printf("   (duplicate/retransmit of sms_number=%u, already relayed - skipping)\n",
               sms_number);
        return;
    }
    sms_mark_handled(sms_number);

    proxy_announce_group(sender_id, talkgroup, text);
    /* No devlog event and no .PING/.TIME auto-reply for group messages -
       not part of what was asked for; add if useful later. */
}

static void handle_packet(const unsigned char *buf, int len) {
    unsigned short sig;

    if (len < 6) return;
    sig = get_u16be(buf);
    if (sig != 0x3242) return;

    if (len == 6) {
        unsigned short frame_type = get_u16be(buf + 2);
        if (frame_type == 0x0001) {
            printf("<- ack for msg_id=%u\n", get_u16be(buf + 4));
        } else if (frame_type == 0x0002) {
            /* printf("<- keepalive echo\n"); */
        } else if (frame_type == 0x0005) {
            printf("<- wakeup echo\n");
        } else {
            printf("<- unknown 6-byte control frame\n");
        }
        return;
    }

    if (len >= 18) {
        unsigned short msg_type = get_u16be(buf + 6);
        unsigned short routing = get_u16be(buf + 16);

        if (len >= 32 && msg_type == 0x8304 && routing == 0x80A1) {
            printf("<- text message frame (%d bytes)\n", len);
            handle_text_message(buf, len);
            return;
        }
        if (len >= 32 && msg_type == 0x8304 && routing == 0x00B1) {
            printf("<- group text message frame (%d bytes)\n", len);
            handle_group_text_message(buf, len);
            return;
        }
        if (len >= 32 && msg_type == 0x8304 && routing == 0x00A2) {
            unsigned char sms_number = buf[0x17];
            unsigned char status = buf[0x20];
            printf("<- delivery-status frame (%d bytes), sms_number=%u, status=0x%02x\n",
                   len, sms_number, status);
            handle_delivery_status(sms_number, status);
            return;
        }
        if (len == 31 && msg_type == 0x8304 && routing == 0x00B2) {
            /* Seen alongside group texts in a real capture - always 31
               bytes, routing 0x00B2. Has a talkgroup-shaped field and an
               incrementing per-talkgroup counter, but no obvious status
               byte (unlike 0x00A2's clear 0x00=delivered/0x08=failed) -
               meaning is NOT understood yet, so it isn't acted on. Given
               its own log line rather than lumped in as "unrecognized"
               now that there's a real, repeatable pattern to point to. */
            printf("<- group session/sequence frame (%d bytes) - meaning not yet understood\n", len);
            return;
        }
    }

    printf("<- unrecognized frame (%d bytes)\n", len);
}

/* ---- main loop ---- */

int main(int argc, char *argv[]) {
    WSADATA wsa;
    fd_set readfds;
    struct timeval tv;
    time_t last_keepalive;
    unsigned char rbuf[1500];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <repeater_ip>\n", argv[0]);
        return 1;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    memset(&g_repeater_addr, 0, sizeof(g_repeater_addr));
    g_repeater_addr.sin_family = AF_INET;
    g_repeater_addr.sin_port = htons(TMS_PORT);
    if (inet_pton(AF_INET, argv[1], &g_repeater_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IP address: %s\n", argv[1]);
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    {
        struct sockaddr_in local_addr;
        int reuse = 1;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(TMS_PORT);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        if (bind(g_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
            fprintf(stderr, "bind() failed: %d (is port %d already in use?)\n",
                    WSAGetLastError(), TMS_PORT);
            closesocket(g_sock);
            WSACleanup();
            return 1;
        }
    }

    /* LAN proxy socket: other software on this PC or the LAN can send/
       receive "[SMS-TS1] {source} to {destination} > {message}\r\n" lines
       here to talk to radios through us. */
    g_proxy_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_proxy_sock == INVALID_SOCKET) {
        fprintf(stderr, "proxy socket() failed: %d\n", WSAGetLastError());
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }
    {
        struct sockaddr_in proxy_local_addr;
        int reuse = 1;
        int broadcast_enable = 1;

        setsockopt(g_proxy_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        setsockopt(g_proxy_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast_enable, sizeof(broadcast_enable));

        memset(&proxy_local_addr, 0, sizeof(proxy_local_addr));
        proxy_local_addr.sin_family = AF_INET;
        proxy_local_addr.sin_port = htons(PROXY_PORT);
        proxy_local_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(g_proxy_sock, (struct sockaddr *)&proxy_local_addr, sizeof(proxy_local_addr)) != 0) {
            fprintf(stderr, "proxy bind() failed: %d (is port %d already in use?)\n",
                    WSAGetLastError(), PROXY_PORT);
            closesocket(g_sock);
            closesocket(g_proxy_sock);
            WSACleanup();
            return 1;
        }

        memset(&g_proxy_broadcast_addr, 0, sizeof(g_proxy_broadcast_addr));
        g_proxy_broadcast_addr.sin_family = AF_INET;
        g_proxy_broadcast_addr.sin_port = htons(PROXY_PORT);
        g_proxy_broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    }

    /* Dev-log broadcast socket: read-only RX/TX/TRY/PASS/FAIL event feed
       for other Hytera programs, same socket conventions as the proxy
       socket / chat.c above, just a separate port. */
    g_devlog_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_devlog_sock == INVALID_SOCKET) {
        fprintf(stderr, "devlog socket() failed: %d\n", WSAGetLastError());
        closesocket(g_sock);
        closesocket(g_proxy_sock);
        WSACleanup();
        return 1;
    }
    {
        struct sockaddr_in devlog_local_addr;
        int reuse = 1;
        int broadcast_enable = 1;

        setsockopt(g_devlog_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        setsockopt(g_devlog_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast_enable, sizeof(broadcast_enable));

        memset(&devlog_local_addr, 0, sizeof(devlog_local_addr));
        devlog_local_addr.sin_family = AF_INET;
        devlog_local_addr.sin_port = htons(DEVLOG_PORT);
        devlog_local_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(g_devlog_sock, (struct sockaddr *)&devlog_local_addr, sizeof(devlog_local_addr)) != 0) {
            fprintf(stderr, "devlog bind() failed: %d (is port %d already in use?)\n",
                    WSAGetLastError(), DEVLOG_PORT);
            closesocket(g_sock);
            closesocket(g_proxy_sock);
            closesocket(g_devlog_sock);
            WSACleanup();
            return 1;
        }

        memset(&g_devlog_broadcast_addr, 0, sizeof(g_devlog_broadcast_addr));
        g_devlog_broadcast_addr.sin_family = AF_INET;
        g_devlog_broadcast_addr.sin_port = htons(DEVLOG_PORT);
        g_devlog_broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    }

    printf("Connecting to repeater %s:%d ...\n", argv[1], TMS_PORT);
    printf("LAN proxy listening on UDP port %d\n", PROXY_PORT);
    printf("Dev-log broadcasting on UDP port %d\n", DEVLOG_PORT);
    send_wakeup();
    last_keepalive = time(NULL);

    for (;;) {
        SOCKET maxfd;

        FD_ZERO(&readfds);
        FD_SET(g_sock, &readfds);
        FD_SET(g_proxy_sock, &readfds);
        maxfd = (g_sock > g_proxy_sock) ? g_sock : g_proxy_sock;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select((int)maxfd + 1, &readfds, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(g_sock, &readfds)) {
            struct sockaddr_in from;
            int fromlen = sizeof(from);
            int n = recvfrom(g_sock, (char *)rbuf, sizeof(rbuf), 0,
                              (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                handle_packet(rbuf, n);
            }
        }

        if (ready > 0 && FD_ISSET(g_proxy_sock, &readfds)) {
            struct sockaddr_in from;
            int fromlen = sizeof(from);
            int n = recvfrom(g_proxy_sock, (char *)rbuf, sizeof(rbuf) - 1,
                              0, (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                rbuf[n] = '\0';
                handle_proxy_line((char *)rbuf);
            }
        }

        if (time(NULL) - last_keepalive >= KEEPALIVE_SECONDS) {
            send_keepalive();
            last_keepalive = time(NULL);
        }

        process_retries();
    }

    closesocket(g_sock);
    closesocket(g_proxy_sock);
    closesocket(g_devlog_sock);
    WSACleanup();
    return 0;
}
