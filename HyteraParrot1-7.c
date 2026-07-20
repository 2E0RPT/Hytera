#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Enable Winsock2 specifications for Windows Vista/7/10+
#endif
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h> // Needed for timeBeginPeriod/timeEndPeriod (precise Sleep() timing)
#define COBJMACROS    // C-style lpVtbl->Method(...) COM calls instead of C++ method syntax
#include <objbase.h>  // WIN32_LEAN_AND_MEAN excludes this from windows.h -- needed explicitly
                      // for CoInitialize/CoCreateInstance/CoUninitialize/CreateStreamOnHGlobal
#include <sapi.h>
// NOTE (MinGW/GCC): #pragma comment(lib, ...) below is an MSVC-only mechanism and is
// silently ignored by MinGW's linker. Compile with explicit -l flags instead:
//   gcc -O2 HyteraParrot1-5.c -o HyteraParrot1-5.exe -lws2_32 -lwinmm -lole32
// -lole32 provides CoInitialize/CoCreateInstance/CoUninitialize/CreateStreamOnHGlobal/
// GetHGlobalFromStream. Getting SAPI's 5 GUIDs to link took three attempts:
//   1. initguid.h trick -- failed, sapi.h apparently doesn't declare these via the
//      DEFINE_GUID macro that trick intercepts.
//   2. Linking -luuid -- failed, MinGW-w64's libuuid.a doesn't bundle SAPI's GUIDs.
//   3. Renaming our own copies (MY_CLSID_SpVoice etc.) -- ALSO failed, with the
//      identical undefined-reference errors still citing the ORIGINAL names. That
//      confirms something (sapi.h itself, or a header it pulls in) references these
//      exact original symbol names internally, regardless of what our own code calls
//      its variables -- renaming our side doesn't satisfy a reference from the header's
//      own side.
// Fix: supply the actual definitions under sapi.h's own original names below (it
// declares them extern; this provides the matching defining declaration in the same
// translation unit, which is standard, legal C and satisfies every reference to them,
// wherever it comes from). Values are the standard, published Microsoft constants for
// these SAPI objects/interfaces.
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

const GUID CLSID_SpVoice       = {0x96749377, 0x3391, 0x11D2, {0x9E,0xE3,0x00,0xC0,0x4F,0x79,0x73,0x96}}; // CONFIRMED correct -- engine init succeeded
const GUID IID_ISpVoice        = {0x6C44DF74, 0x72B9, 0x4992, {0xA1,0xEC,0xEF,0x99,0x6E,0x04,0x22,0xD4}}; // CONFIRMED correct -- engine init succeeded
// CLSID_SpStream removed: the hand-typed value here was confirmed WRONG on
// real hardware (CoCreateInstance returned 0x80040154, REGDB_E_CLASSNOTREG).
// Now resolved dynamically via CLSIDFromProgID(L"SAPI.SpStream", ...) instead
// -- see synthesize_report_to_frames() below.
const GUID IID_ISpStream       = {0x12e3cca9, 0x7518, 0x44c5, {0xa5,0xe7, 0xba,0x5a,0x79,0xcb,0x92,0x9e}}; // CONFIRMED correct -- found verbatim (DEFINE_GUID) in user's own sapi51.h/sapi53.h/sapi54.h
const GUID SPDFID_WaveFormatEx = {0xC31ADBAE, 0x527F, 0x4FF5, {0xA2,0x30,0xF6,0x2B,0xB6,0x1F,0xF7,0x0C}}; // still unverified -- user's headers only have "EXTERN_C const GUID SPDFID_WaveFormatEx;" with no DEFINE_GUID anywhere, so this genuinely can't be grep-checked against their system the way IID_ISpStream was. If SetBaseStream below fails, this value is the next suspect.

// ==========================================================
// HyteraParrot -- field test tool.
//
// Listens for a station transmitting through the repeater, buffers their
// audio, and once the transmission has been truly silent for 5 seconds,
// plays it straight back to them through the repeater -- lets an operator
// key up, walk to a test location, and hear exactly what the repeater
// actually received from them.
//
// The 5 second wait (not just "de-key detected") is deliberate: a weak
// or fading signal can drop out for a moment and come back. If that
// happens within the 5 second window, the new audio is appended to the
// SAME recording rather than starting a fresh one or playing back a
// truncated clip -- with the correct amount of silence inserted for the
// gap, so the played-back timing matches what actually happened on air.
//
// Unlike HyteraTransceiver, this never touches the sound card at all --
// no mic, no speakers. It stores and replays the raw G.711 u-law bytes
// exactly as they came off the wire, with no PCM decode/re-encode round
// trip. That's both simpler and a more honest test of what the repeater
// itself did to the audio.
//
// NOTE: this is also a good way to finally settle an open question from
// this project -- RX parses incoming packets with a 29-byte header,
// while our own outgoing TX header is 28 bytes, and that 1-byte gap was
// never fully confirmed as intentional. If the echoed audio sounds
// clean, that split is fine as-is; if it sounds subtly shifted/garbled,
// that mismatch is the prime suspect.
// ==========================================================

#define LOCAL_IP             "192.168.1.136" // Your PC IP
#define PORT_RCP              30009          // Radio Control Port (TX only)
#define PORT_RTP              30012          // Voice/RTP Port (shared RX + TX)
#define TARGET_TALKGROUP      1
#define CALL_TYPE_GROUP       1

#define PAYLOAD_OFFSET        28             // CORRECTED from 29 -- confirmed by direct pcap
                                              // measurement: real downlink packets are exactly
                                              // 508 bytes = 28(header) + 480(3x160 audio frames),
                                              // and playback packets are 188 = 28+160. With 29
                                              // neither divides evenly by 160. This also settles
                                              // the old "28 vs 29" question noted in project notes.
#define NETWORK_BUF_SZ        2048
#define FRAME_BYTES           160            // One 20ms G.711 u-law frame
#define SILENCE_BYTE          0xFF           // Confirmed correct u-law silence encoding

#define SILENCE_TIMEOUT_MS    5000           // How long the channel must be truly quiet before we play back
#define MAX_RECORD_SECONDS    300            // 5 minute cap per capture
#define MAX_FRAMES            (MAX_RECORD_SECONDS * 50) // 50 frames/sec at 20ms each

const uint8_t WAKE_CALL_PAYLOAD[]   = {0x32, 0x42, 0x00, 0x05, 0x00, 0x00};
const uint8_t KEEP_ALIVE_PAYLOAD[]  = {0x32, 0x42, 0x00, 0x02, 0x00, 0x00};

const uint8_t CALL_SETUP_TEMPLATE[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x41, 0x08, 0x05, 0x00, 0x01,
    0x7C, 0x09, 0x00, 0x00, 0x5E, 0x03
};

const uint8_t PTT_TEMPLATE[] = {
    0x32, 0x42, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x41, 0x00, 0x02, 0x00, 0x03,
    0x00, 0x00, 0x03
};

#pragma pack(push, 1)
typedef struct {
    uint16_t fixed_marker;
    uint16_t seq_num;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t  hytera_pad[16];
    uint8_t  voice_payload[FRAME_BYTES];
} rtp_packet_t;
#pragma pack(pop)

typedef struct {
    SOCKET socket;
    struct sockaddr_in target_addr;
} keepalive_ctx_t;

static uint8_t  rcp_sequence_counter = 0;
static uint16_t rtp_sequence_counter = 0;
static uint32_t rtp_timestamp_counter = 0;

// The capture buffer -- raw u-law bytes, one 160-byte frame per slot.
static uint8_t frame_buffer[MAX_FRAMES][FRAME_BYTES];
static int frame_count = 0;

// ==========================================================
// FEEDBACK REPORT -- spoken summary played after the echoed audio,
// covering volume and packet loss (signal-strength/RSSI is deferred to
// later, once SNMP telemetry is wired in here -- see the TODO below).
// ==========================================================

// u-law decode LUT -- only used here to compute average volume for the
// report. The actual captured/replayed audio stays raw-byte passthrough
// as before; this is a separate, read-only analysis pass over the same
// bytes, ported verbatim from HyteraTransceiver/HyteraGUI.
static int16_t ulaw_to_pcm_lut[256];
void init_ulaw_lut(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t data = ~i;
        int sign = (data & 0x80);
        int exponent = (data >> 4) & 0x07;
        int mantissa = data & 0x0F;
        int sample = ((mantissa << 3) + 33) << exponent;
        sample -= 33;
        ulaw_to_pcm_lut[i] = (int16_t)(sign ? -sample : sample);
    }
}

// Standard G.711 linear-PCM-to-u-law encoder, needed to turn SAPI's PCM
// speech output into the same raw u-law bytes as everything else this
// program transmits. Ported verbatim from HyteraTransceiver/HyteraGUI.
#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635
uint8_t linear_to_ulaw(int16_t pcm_val) {
    int sign = (pcm_val >> 8) & 0x80;
    if (sign) pcm_val = (int16_t)(-pcm_val);
    if (pcm_val > ULAW_CLIP) pcm_val = ULAW_CLIP;
    pcm_val += ULAW_BIAS;
    int exponent = 7;
    for (int expMask = 0x4000; (pcm_val & expMask) == 0 && exponent > 0; expMask >>= 1, exponent--);
    int mantissa = (pcm_val >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

// Volume classification thresholds -- these are a starting-point heuristic,
// not a calibrated standard. Measured against average |PCM amplitude| after
// u-law decode (0-32767 range). The existing VU meters elsewhere in this
// project normalize against 8000 as a "reasonably loud, full bar" reference
// point, so these tiers are built around that same scale for consistency.
// Adjust freely once you've heard a few real reports.
#define VOL_THRESH_VERY_QUIET  200.0f
#define VOL_THRESH_QUIET       400.0f
#define VOL_THRESH_GOOD        1200.0f
#define VOL_THRESH_LOUD        2000.0f

// Score penalties -- also a starting-point heuristic. Score currently only
// reflects packet loss and volume, since signal strength (RSSI via SNMP)
// is deliberately deferred -- see TODO in build_feedback_report_text().
#define SCORE_PENALTY_VOLUME_MINOR  5.0f
#define SCORE_PENALTY_VOLUME_MAJOR 50.0f

// Report audio gets its own small frame buffer -- a spoken sentence is a
// few seconds at most, nowhere near the main MAX_FRAMES capture cap.
#define MAX_REPORT_FRAMES  (15 * 50) // 15 seconds ceiling, generous for one sentence
static uint8_t report_frame_buffer[MAX_REPORT_FRAMES][FRAME_BYTES];
static int report_frame_count = 0;

// SAPI voice, created once at startup and reused for every report.
static ISpVoice* g_pVoice = NULL;
static int g_tts_available = 0;

int init_tts(void) {
    HRESULT hr = CoInitialize(NULL);
	printf("[AUTHOR] HyteraParrot made by Rob Thompson 2E0RPT...\n");
    if (FAILED(hr)) {
        printf("[TTS] CoInitialize failed (0x%08lX) -- spoken feedback reports disabled.\n", hr);
        return 0;
    }
    hr = CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void**)&g_pVoice);
    if (FAILED(hr)) {
        printf("[TTS] Could not create SAPI voice (0x%08lX) -- spoken feedback reports disabled.\n", hr);
        CoUninitialize();
        return 0;
    }
    g_tts_available = 1;
    printf("[TTS] Speech engine ready -- feedback reports will be spoken after each playback.\n");
    return 1;
}

void shutdown_tts(void) {
    if (g_pVoice) { g_pVoice->lpVtbl->Release(g_pVoice); g_pVoice = NULL; }
    if (g_tts_available) CoUninitialize();
    g_tts_available = 0;
}

// Computes the average absolute PCM amplitude across the current capture,
// for the volume portion of the report.
float compute_average_volume(void) {
    if (frame_count <= 0) return 0.0f;
    double sum = 0.0;
    long long count = 0;
    for (int f = 0; f < frame_count; f++) {
        for (int b = 0; b < FRAME_BYTES; b++) {
            sum += abs(ulaw_to_pcm_lut[frame_buffer[f][b]]);
            count++;
        }
    }
    return (count > 0) ? (float)(sum / (double)count) : 0.0f;
}

const char* classify_volume(float avg_volume) {
    if (avg_volume < VOL_THRESH_VERY_QUIET) return "too quiet. Please speak louder.";
    if (avg_volume < VOL_THRESH_QUIET)      return "slightly too quiet.";
    if (avg_volume < VOL_THRESH_GOOD)       return "a good level.";
    if (avg_volume < VOL_THRESH_LOUD)       return "slightly too loud.";
    return "too loud. Please speak quietly.";
}

// Builds the spoken report sentence and the numeric score together, so
// the score calculation and the words describing it can't drift apart.
//
// TODO (deferred, needs SNMP): once RSSI/signal-strength telemetry is
// wired into this program, prepend a clause here like "Your signal was
// S9 plus 20." and fold a signal-quality term into the score below.
// For now the score only reflects volume and packet loss.
void build_feedback_report_text(wchar_t* out_text, int out_text_size, float loss_pct, float* out_score) {
    if (loss_pct < 0.0f) loss_pct = 0.0f; // small negative values are just measurement rounding
    if (loss_pct > 100.0f) loss_pct = 100.0f;

    float avg_volume = compute_average_volume();
    const char* volume_desc = classify_volume(avg_volume);

    float volume_penalty = 0.0f;
    if (avg_volume < VOL_THRESH_VERY_QUIET || avg_volume >= VOL_THRESH_LOUD) {
        volume_penalty = SCORE_PENALTY_VOLUME_MAJOR;
    } else if (avg_volume < VOL_THRESH_QUIET || avg_volume >= VOL_THRESH_GOOD) {
        volume_penalty = SCORE_PENALTY_VOLUME_MINOR;
    }

    float score = 100.0f - loss_pct - volume_penalty;
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    *out_score = score;

    wchar_t wVolumeDesc[32];
    MultiByteToWideChar(CP_ACP, 0, volume_desc, -1, wVolumeDesc, 32);

    _snwprintf(out_text, out_text_size,
        L"Your audio was %s, and you have %.0f percent packets lost. Your overall score is %.0f percent.",
        wVolumeDesc, loss_pct, score);
    out_text[out_text_size - 1] = L'\0';
}

// Synthesizes text via SAPI and converts the result into our own raw u-law
// frame buffer for transmission -- reads SAPI's real synthesized audio via
// an in-memory stream (CreateStreamOnHGlobal), NOT a placeholder/test tone.
// Requests 22050Hz/16-bit/mono PCM from SAPI (a format essentially every
// installed voice supports), then resamples down to 8000Hz ourselves via
// linear interpolation and re-encodes to u-law with linear_to_ulaw().
// Returns the number of frames written into report_frame_buffer (0 on
// any failure -- caller should just skip the spoken report in that case).
int synthesize_report_to_frames(const wchar_t* text) {
    if (!g_tts_available) return 0;

    IStream* pMemStream = NULL;
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &pMemStream);
    if (FAILED(hr) || !pMemStream) {
        printf("[TTS] Could not create memory stream (0x%08lX) -- skipping spoken report.\n", hr);
        return 0;
    }

    // CLSID_SpStream's hand-typed value was confirmed wrong on real hardware
    // (0x80040154 = REGDB_E_CLASSNOTREG). Rather than guess another hex
    // constant, resolve the class dynamically via its ProgID -- a plain
    // text string mapped in the registry, far less room for a silent typo
    // than 32 hex digits, and Windows does the lookup for us.
    CLSID clsidSpStream;
    hr = CLSIDFromProgID(L"SAPI.SpStream", &clsidSpStream);
    if (FAILED(hr)) {
        printf("[TTS] Could not resolve SAPI.SpStream ProgID (0x%08lX) -- skipping spoken report.\n", hr);
        pMemStream->lpVtbl->Release(pMemStream);
        return 0;
    }

    ISpStream* pSpStream = NULL;
    hr = CoCreateInstance(&clsidSpStream, NULL, CLSCTX_INPROC_SERVER, &IID_ISpStream, (void**)&pSpStream);
    if (FAILED(hr) || !pSpStream) {
        printf("[TTS] Could not create SAPI stream wrapper (0x%08lX) -- skipping spoken report.\n", hr);
        pMemStream->lpVtbl->Release(pMemStream);
        return 0;
    }

    WAVEFORMATEX wfxTTS;
    memset(&wfxTTS, 0, sizeof(wfxTTS));
    wfxTTS.wFormatTag      = WAVE_FORMAT_PCM;
    wfxTTS.nChannels       = 1;
    wfxTTS.nSamplesPerSec  = 22050;
    wfxTTS.wBitsPerSample  = 16;
    wfxTTS.nBlockAlign     = (wfxTTS.nChannels * wfxTTS.wBitsPerSample) / 8;
    wfxTTS.nAvgBytesPerSec = wfxTTS.nSamplesPerSec * wfxTTS.nBlockAlign;
    wfxTTS.cbSize          = 0;

    hr = pSpStream->lpVtbl->SetBaseStream(pSpStream, pMemStream, &SPDFID_WaveFormatEx, &wfxTTS);
    if (FAILED(hr)) {
        printf("[TTS] SetBaseStream failed (0x%08lX) -- skipping spoken report.\n", hr);
        pSpStream->lpVtbl->Release(pSpStream);
        pMemStream->lpVtbl->Release(pMemStream);
        return 0;
    }

    hr = g_pVoice->lpVtbl->SetOutput(g_pVoice, (IUnknown*)pSpStream, TRUE);
    if (FAILED(hr)) {
        printf("[TTS] SetOutput failed (0x%08lX) -- skipping spoken report.\n", hr);
        pSpStream->lpVtbl->Release(pSpStream);
        pMemStream->lpVtbl->Release(pMemStream);
        return 0;
    }

    // Synchronous (no SPF_ASYNC) -- blocks here until speech is fully
    // rendered into pMemStream, which is exactly what we want before we
    // go read it back.
    hr = g_pVoice->lpVtbl->Speak(g_pVoice, text, SPF_DEFAULT, NULL);
    pSpStream->lpVtbl->Release(pSpStream); // flush; pMemStream itself stays alive, separate refcount

    if (FAILED(hr)) {
        printf("[TTS] Speak failed (0x%08lX) -- skipping spoken report.\n", hr);
        pMemStream->lpVtbl->Release(pMemStream);
        return 0;
    }

    LARGE_INTEGER seekZero; seekZero.QuadPart = 0;
    ULARGE_INTEGER streamSize;
    pMemStream->lpVtbl->Seek(pMemStream, seekZero, STREAM_SEEK_END, &streamSize);
    DWORD totalBytes = (DWORD)streamSize.QuadPart;

    HGLOBAL hGlobal = NULL;
    GetHGlobalFromStream(pMemStream, &hGlobal);
    uint8_t* rawWav = (uint8_t*)GlobalLock(hGlobal);

    int frames_written = 0;

    // ISpStream::SetBaseStream does NOT wrap the output in a RIFF/WAVE
    // container the way BindToFile does for real .wav files -- it just
    // writes raw PCM samples in the format we specified (22050Hz/16-bit/
    // mono) directly to the stream. Confirmed by testing: totalBytes came
    // back as a real, substantial value (not 0, not garbage) that matches
    // a plausible raw-PCM duration for the spoken sentence, but never
    // started with "RIFF"/"WAVE" markers. So we just treat the whole
    // buffer as raw PCM16 directly -- no container to parse.
    if (rawWav && totalBytes >= 2) {
        int16_t* pcm16 = (int16_t*)rawWav;
        uint32_t numSamples = totalBytes / 2;
        printf("[TTS] SAPI returned %u raw samples (%.2fs at 22050Hz).\n", numSamples, numSamples / 22050.0f);

        double ratio = 22050.0 / 8000.0;
        int outSampleCount = (int)(numSamples / ratio);
        int maxOutSamples = MAX_REPORT_FRAMES * FRAME_BYTES;
        if (outSampleCount > maxOutSamples) outSampleCount = maxOutSamples;

        uint8_t current_frame[FRAME_BYTES];
        int current_frame_pos = 0;

        for (int i = 0; i < outSampleCount; i++) {
            double srcPos = i * ratio;
            int idx0 = (int)srcPos;
            int idx1 = idx0 + 1;
            if (idx1 >= (int)numSamples) idx1 = (int)numSamples - 1;
            double frac = srcPos - idx0;
            int16_t s0 = pcm16[idx0];
            int16_t s1 = pcm16[idx1];
            int16_t interpolated = (int16_t)(s0 + (double)(s1 - s0) * frac);

            current_frame[current_frame_pos++] = linear_to_ulaw(interpolated);
            if (current_frame_pos == FRAME_BYTES) {
                if (frames_written < MAX_REPORT_FRAMES) {
                    memcpy(report_frame_buffer[frames_written], current_frame, FRAME_BYTES);
                    frames_written++;
                }
                current_frame_pos = 0;
            }
        }
        if (current_frame_pos > 0 && frames_written < MAX_REPORT_FRAMES) {
            memset(current_frame + current_frame_pos, SILENCE_BYTE, FRAME_BYTES - current_frame_pos);
            memcpy(report_frame_buffer[frames_written], current_frame, FRAME_BYTES);
            frames_written++;
        }
        if (frames_written == 0) {
            printf("[TTS] Resampling produced 0 frames from %u raw samples -- skipping spoken report.\n", numSamples);
        }
    } else {
        printf("[TTS] SAPI's output stream was empty (totalBytes=%lu) -- skipping spoken report.\n", totalBytes);
    }

    if (rawWav) GlobalUnlock(hGlobal);
    pMemStream->lpVtbl->Release(pMemStream);
    return frames_written;
}

uint8_t get_next_rcp_seq() { return rcp_sequence_counter++; }

void send_call_setup(SOCKET sock, struct sockaddr_in* addr, uint8_t call_type, uint32_t target_id) {
    uint8_t packet[sizeof(CALL_SETUP_TEMPLATE)];
    memcpy(packet, CALL_SETUP_TEMPLATE, sizeof(CALL_SETUP_TEMPLATE));
    packet[5]  = get_next_rcp_seq();
    packet[11] = call_type;
    packet[12] = target_id & 0xFF;
    packet[13] = (target_id >> 8) & 0xFF;
    packet[14] = (target_id >> 16) & 0xFF;
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

void send_ptt_command(SOCKET sock, struct sockaddr_in* addr, int turn_on) {
    uint8_t packet[sizeof(PTT_TEMPLATE)];
    memcpy(packet, PTT_TEMPLATE, sizeof(PTT_TEMPLATE));
    packet[5] = get_next_rcp_seq();
    if (turn_on) { packet[12] = 0x01; packet[13] = 0xEB; }
    else         { packet[12] = 0x00; packet[13] = 0xEC; }
    sendto(sock, (const char*)packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr));
}

VOID CALLBACK SendKeepaliveCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer) {
    keepalive_ctx_t *ctx = (keepalive_ctx_t*)Context;
    sendto(ctx->socket, (const char*)KEEP_ALIVE_PAYLOAD, sizeof(KEEP_ALIVE_PAYLOAD), 0,
           (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

// Streams the captured frames back through the repeater at the correct
// 20ms cadence -- same Call Setup / PTT pattern as HyteraTransceiver's TX
// side, just sourcing frames from our buffer instead of a live mic.
// Sends one 160-byte u-law frame as an RTP packet -- shared by both the
// captured-audio playback and the spoken-report playback below.
void send_one_frame(SOCKET rtp_sock, struct sockaddr_in* remote_rtp_addr, const uint8_t* frame) {
    rtp_packet_t audio_pkt;
    memset(&audio_pkt, 0, sizeof(audio_pkt));

    audio_pkt.fixed_marker = htons(0x9000);
    audio_pkt.seq_num      = htons(rtp_sequence_counter++);
    audio_pkt.timestamp    = htonl(rtp_timestamp_counter);
    audio_pkt.ssrc         = 0;
    audio_pkt.hytera_pad[1] = 0x15;
    audio_pkt.hytera_pad[3] = 0x03;

    memcpy(audio_pkt.voice_payload, frame, FRAME_BYTES);

    sendto(rtp_sock, (const char*)&audio_pkt, sizeof(audio_pkt), 0,
           (struct sockaddr*)remote_rtp_addr, sizeof(*remote_rtp_addr));

    rtp_timestamp_counter += FRAME_BYTES;
    Sleep(20);
}

// loss_pct is passed in already computed by the caller (from the same
// diagnostic numbers it prints), so the spoken report's score can't drift
// from the printed diagnostic.
void play_back_capture(SOCKET rcp_sock, SOCKET rtp_sock, struct sockaddr_in* remote_rcp_addr, struct sockaddr_in* remote_rtp_addr, float loss_pct) {
    printf("[PLAYBACK] Silence confirmed -- playing back %d frames (%.1f seconds) captured audio...\n",
           frame_count, frame_count * 0.02f);

    // Build the spoken feedback report now, before keying up, so if TTS
    // synthesis is slow it doesn't add dead air mid-transmission.
    wchar_t report_text[256];
    float score = 0.0f;
    build_feedback_report_text(report_text, 256, loss_pct, &score);
    report_frame_count = synthesize_report_to_frames(report_text);
    if (report_frame_count > 0) {
        printf("[TTS] Feedback report ready (%d frames, score %.0f%%).\n", report_frame_count, score);
    } else {
        printf("[TTS] No spoken report this time (score would have been %.0f%%) -- see [TTS] messages above for why.\n", score);
    }

    printf("[RCP] Sending Call Setup for Talkgroup %d...\n", TARGET_TALKGROUP);
    send_call_setup(rcp_sock, remote_rcp_addr, CALL_TYPE_GROUP, TARGET_TALKGROUP);
    Sleep(100);

    printf("[RCP] PTT Key-Up (playback)...\n");
    send_ptt_command(rcp_sock, remote_rcp_addr, 1);
    Sleep(100);

    for (int i = 0; i < frame_count; i++) {
        send_one_frame(rtp_sock, remote_rtp_addr, frame_buffer[i]);
    }

    if (report_frame_count > 0) {
        // Short pause between the echoed audio and the spoken report so
        // they don't run together.
        uint8_t silence_frame[FRAME_BYTES];
        memset(silence_frame, SILENCE_BYTE, FRAME_BYTES);
        for (int s = 0; s < 20; s++) { // 20 x 20ms = 0.4s
            send_one_frame(rtp_sock, remote_rtp_addr, silence_frame);
        }

        printf("[TTS] Speaking feedback report...\n");
        for (int i = 0; i < report_frame_count; i++) {
            send_one_frame(rtp_sock, remote_rtp_addr, report_frame_buffer[i]);
        }
    }

    printf("[RCP] PTT De-Key (playback finished)...\n");
    send_ptt_command(rcp_sock, remote_rcp_addr, 0);
    printf("[SYSTEM] Playback complete. Listening for the next transmission...\n\n");
}

int main(int argc, char *argv[]) {
    // Without this, Windows' default timer granularity (~15.6ms) makes
    // Sleep(20) imprecise/jittery -- confirmed by comparison against a
    // real pcap capture that our own playback timing was the remaining
    // source of fragmentation after the frame-bundling fix. Same fix
    // HyteraTransceiver already uses for its own Sleep(20)-paced TX.
    timeBeginPeriod(1);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Winsock initialization failed.\n");
        timeEndPeriod(1);
        return -1;
    }

    init_ulaw_lut();
    init_tts(); // Failure here just disables spoken reports, not fatal -- checked via g_tts_available

    SOCKET rcp_sock, rtp_sock;
    struct sockaddr_in local_rcp_addr, local_rtp_addr;
    struct sockaddr_in remote_rcp_addr, remote_rtp_addr;

    if ((rcp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET ||
        (rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        printf("[ERROR] Socket creation failed.\n");
        shutdown_tts();
        WSACleanup();
        timeEndPeriod(1);
        return -1;
    }

    // Short receive timeout so the main loop can periodically check the
    // 5-second silence timeout even when no packets are arriving at all.
    DWORD rx_timeout = 100;
    setsockopt(rtp_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rx_timeout, sizeof(rx_timeout));

    // Larger receive buffer than the Windows default -- if this loop is
    // ever even briefly delayed (console I/O, OS scheduling), a bigger
    // buffer gives incoming packets somewhere to queue instead of the OS
    // silently discarding them. Cheap, safe, standard practice for a
    // steady real-time UDP stream.
    int rcvbuf_size = 262144; // 256KB
    setsockopt(rtp_sock, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf_size, sizeof(rcvbuf_size));

    memset(&local_rcp_addr, 0, sizeof(local_rcp_addr));
    local_rcp_addr.sin_family = AF_INET;
    local_rcp_addr.sin_port = htons(PORT_RCP);
    inet_pton(AF_INET, LOCAL_IP, &local_rcp_addr.sin_addr);

    memset(&local_rtp_addr, 0, sizeof(local_rtp_addr));
    local_rtp_addr.sin_family = AF_INET;
    local_rtp_addr.sin_port = htons(PORT_RTP);
    local_rtp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(rcp_sock, (struct sockaddr*)&local_rcp_addr, sizeof(local_rcp_addr)) == SOCKET_ERROR ||
        bind(rtp_sock, (struct sockaddr*)&local_rtp_addr, sizeof(local_rtp_addr)) == SOCKET_ERROR) {
        printf("[ERROR] Socket port bindings failed.\n");
        shutdown_tts();
        closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup();
        timeEndPeriod(1);
        return -1;
    }

    //printf("[AUTHOR] HyteraParrot made by Rob Thompson 2E0RPT...\n");

    // ------------------------------------------------------------
    // Resolve the repeater's IP: argv[1], or wait for it to speak first --
    // same pattern as HyteraTransceiver.
    // ------------------------------------------------------------
    char repeater_ip_str[INET_ADDRSTRLEN];
    if (argc >= 2) {
        struct in_addr testAddr;
        if (inet_pton(AF_INET, argv[1], &testAddr) != 1) {
            printf("[ERROR] '%s' is not a valid IPv4 address.\n", argv[1]);
            printf("        Usage: %s [repeater_ip]\n", argv[0]);
            shutdown_tts();
            closesocket(rcp_sock); closesocket(rtp_sock); WSACleanup();
            timeEndPeriod(1);
            return -1;
        }
        strncpy(repeater_ip_str, argv[1], sizeof(repeater_ip_str) - 1);
        repeater_ip_str[sizeof(repeater_ip_str) - 1] = '\0';
        printf("[SYSTEM] Using repeater IP from command line: %s\n", repeater_ip_str);
    } else {
        printf("[SYSTEM] No repeater IP given -- waiting for the repeater to contact us on port %d...\n\n", PORT_RTP);
        struct sockaddr_in discovery_addr;
        int discovery_addr_len = sizeof(discovery_addr);
        char discovery_buf[NETWORK_BUF_SZ];
        int waited_ms = 0;
        while (1) {
            discovery_addr_len = sizeof(discovery_addr);
            int n = recvfrom(rtp_sock, discovery_buf, sizeof(discovery_buf), 0,
                              (struct sockaddr*)&discovery_addr, &discovery_addr_len);
            if (n > 0) {
                inet_ntop(AF_INET, &discovery_addr.sin_addr, repeater_ip_str, sizeof(repeater_ip_str));
                printf("[SYSTEM] Discovered repeater IP: %s\n", repeater_ip_str);
                break;
            }
            waited_ms += 100;
            if (waited_ms % 5000 == 0) {
                printf("[SYSTEM] Still waiting for the repeater to speak first (%d s)...\n", waited_ms / 1000);
            }
        }
    }

    memset(&remote_rcp_addr, 0, sizeof(remote_rcp_addr));
    remote_rcp_addr.sin_family = AF_INET;
    remote_rcp_addr.sin_port = htons(PORT_RCP);
    inet_pton(AF_INET, repeater_ip_str, &remote_rcp_addr.sin_addr);

    memset(&remote_rtp_addr, 0, sizeof(remote_rtp_addr));
    remote_rtp_addr.sin_family = AF_INET;
    remote_rtp_addr.sin_port = htons(PORT_RTP);
    inet_pton(AF_INET, repeater_ip_str, &remote_rtp_addr.sin_addr);

    printf("[SYSTEM] Launching hardware link wake sequence...\n");
    sendto(rcp_sock, (const char*)WAKE_CALL_PAYLOAD, sizeof(WAKE_CALL_PAYLOAD), 0, (struct sockaddr*)&remote_rcp_addr, sizeof(remote_rcp_addr));
    sendto(rtp_sock, (const char*)WAKE_CALL_PAYLOAD, sizeof(WAKE_CALL_PAYLOAD), 0, (struct sockaddr*)&remote_rtp_addr, sizeof(remote_rtp_addr));

    keepalive_ctx_t rcp_ctx = { rcp_sock, remote_rcp_addr };
    keepalive_ctx_t rtp_ctx = { rtp_sock, remote_rtp_addr };
    PTP_TIMER rcp_timer = CreateThreadpoolTimer(SendKeepaliveCallback, &rcp_ctx, NULL);
    PTP_TIMER rtp_timer = CreateThreadpoolTimer(SendKeepaliveCallback, &rtp_ctx, NULL);
    if (rcp_timer != NULL && rtp_timer != NULL) {
        FILETIME ftDueTime; ULARGE_INTEGER ulDueTime; ulDueTime.QuadPart = 0;
        ftDueTime.dwHighDateTime = ulDueTime.HighPart; ftDueTime.dwLowDateTime = ulDueTime.LowPart;
        SetThreadpoolTimer(rcp_timer, &ftDueTime, 5000, 0);
        SetThreadpoolTimer(rtp_timer, &ftDueTime, 5000, 0);
        printf("[SYSTEM] Asynchronous 5-second keepalives armed.\n");
    }

    printf("[SYSTEM] Holding link stabilization window for 4 seconds...\n");
    Sleep(4000);

    rtp_sequence_counter = (uint16_t)time(NULL);
    rtp_timestamp_counter = (uint32_t)rtp_sequence_counter;

    printf("[SYSTEM] Ready. Listening for a station to transmit. Press ESC to quit.\n\n");

    // ------------------------------------------------------------
    // Main capture/playback loop
    // ------------------------------------------------------------
    int recording = 0;
    int max_length_warned = 0;
    DWORD last_packet_time = 0;
    DWORD first_packet_time = 0;
    uint32_t current_radio_id = 0;

    char net_buffer[NETWORK_BUF_SZ];
    struct sockaddr_in client_addr;
    int client_addr_len;

    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            printf("[SYSTEM] Exiting.\n");
            break;
        }

        client_addr_len = sizeof(client_addr);
        int bytes_received = recvfrom(rtp_sock, net_buffer, NETWORK_BUF_SZ, 0,
                                      (struct sockaddr *)&client_addr, &client_addr_len);
        DWORD now = GetTickCount();

        if (bytes_received > PAYLOAD_OFFSET) {
            // A real audio-bearing packet arrived.
            uint8_t *ubuf = (uint8_t *)net_buffer;
            uint32_t parsed_id = 0;
            if (bytes_received >= 23) {
                parsed_id = ((uint32_t)ubuf[17] << 16) | ((uint32_t)ubuf[18] << 8) | (uint32_t)ubuf[19];
            }

            int payload_len = bytes_received - PAYLOAD_OFFSET;

            if (!recording) {
                printf("[CAPTURE] Station detected (Radio ID %u) -- recording...\n", parsed_id);
                frame_count = 0;
                recording = 1;
                current_radio_id = parsed_id;
                first_packet_time = now;
                max_length_warned = 0;
            } else {
                // Back-fill silence for the gap since the last packet --
                // this is what makes a momentary fade-out/fade-back-in
                // sound correct on playback instead of clipped together.
                DWORD gap_ms = now - last_packet_time;
                // 300ms, not 40ms: real-world testing showed gaps of up to
                // ~100ms happening on nearly every frame transition even
                // during unbroken speech -- ordinary network/OS jitter, not
                // a real dropout. Treating those as gaps and backfilling
                // silence for them chopped continuous speech into audible
                // stutters. 300ms comfortably clears that noise floor while
                // still catching genuinely perceptible fades. (Still a safe
                // margin now that packets can arrive every ~60ms instead of
                // every 20ms, since the repeater bundles 3 frames/packet.)
                if (gap_ms > 300) {
                    int silence_frames = gap_ms / 20;
                    for (int s = 0; s < silence_frames && frame_count < MAX_FRAMES; s++) {
                        memset(frame_buffer[frame_count], SILENCE_BYTE, FRAME_BYTES);
                        frame_count++;
                    }
                    printf("[CAPTURE] Signal dropped out for %.1fs and came back -- inserted matching silence.\n", gap_ms / 1000.0f);
                }
            }

            // The repeater bundles multiple 20ms frames into one UDP packet
            // (confirmed via pcap: downlink packets are 480 bytes of audio =
            // 3 frames, not 1) -- extract every full frame present, not just
            // the first. This was the actual bug behind the "stuttery/faster"
            // playback: 2 out of every 3 frames were being silently dropped.
            int num_frames_in_packet = payload_len / FRAME_BYTES;
            int remainder_bytes = payload_len % FRAME_BYTES;

            for (int fnum = 0; fnum < num_frames_in_packet && frame_count < MAX_FRAMES; fnum++) {
                memcpy(frame_buffer[frame_count], ubuf + PAYLOAD_OFFSET + (fnum * FRAME_BYTES), FRAME_BYTES);
                frame_count++;
            }
            if (remainder_bytes > 0 && frame_count < MAX_FRAMES) {
                // Trailing partial frame (e.g. the last frame of a
                // transmission cut slightly short) -- pad with silence.
                uint8_t *frame = frame_buffer[frame_count];
                memcpy(frame, ubuf + PAYLOAD_OFFSET + (num_frames_in_packet * FRAME_BYTES), remainder_bytes);
                memset(frame + remainder_bytes, SILENCE_BYTE, FRAME_BYTES - remainder_bytes);
                frame_count++;
            }
            if (frame_count >= MAX_FRAMES && !max_length_warned) {
                printf("[WARNING] Max capture length (%d seconds) reached -- further audio in this transmission will be dropped, but timing/playback will still proceed normally.\n", MAX_RECORD_SECONDS);
                max_length_warned = 1;
            }

            last_packet_time = now;
        }

/*         if (recording && (now - last_packet_time) >= SILENCE_TIMEOUT_MS) {
            recording = 0;
            if (frame_count > 0) {
                // Diagnostic: compare how many frames we'd expect for the
                // real elapsed time (first packet to last packet, at one
                // frame per 20ms) against how many we actually captured.
                // A large gap between these numbers confirms real packet
                // loss during capture, rather than a timing/logic bug.
                DWORD real_span_ms = last_packet_time - first_packet_time;
                int expected_frames = (real_span_ms / 20) + 1;
                float loss_pct = 0.0f;
                if (expected_frames > 0) {
                    loss_pct = 100.0f * (1.0f - ((float)frame_count / (float)expected_frames));
                    //printf("[DIAG] Real transmission spanned %lums -- expected ~%d frames, captured %d (%.0f%% apparent loss).\n",
					printf("[DIAG] Real transmission spanned %lums -- expected ~%d frames, captured %d (%.0f%% apparent loss) -- Avg Vol: %.2f.\n",transmission_time, expected_frames, captured_frames, loss_percentage, avg_volume);

                           real_span_ms, expected_frames, frame_count, loss_pct);
                }
                play_back_capture(rcp_sock, rtp_sock, &remote_rcp_addr, &remote_rtp_addr, loss_pct);
            }
            frame_count = 0;
        } */
		if (recording && (now - last_packet_time) >= SILENCE_TIMEOUT_MS) {
			recording = 0;
			if (frame_count > 0) {
				// Diagnostic: compare how many frames we'd expect for the
				// real elapsed time (first packet to last packet, at one
				// frame per 20ms) against how many we actually captured.
				// A large gap between these numbers confirms real packet
				// loss during capture, rather than a timing/logic bug.
				DWORD real_span_ms = last_packet_time - first_packet_time;
				int expected_frames = (real_span_ms / 20) + 1;
				float loss_pct = 0.0f;
				if (expected_frames > 0) {
					loss_pct = 100.0f * (1.0f - ((float)frame_count / (float)expected_frames));
					
					// We call compute_average_volume() directly inside the printf arguments
					printf("[DIAG] Real transmission spanned %lums -- expected ~%d frames, captured %d (%.0f%% apparent loss), Avg Vol: %.1f.\n",
						   real_span_ms, expected_frames, frame_count, loss_pct, compute_average_volume());
				}
				play_back_capture(rcp_sock, rtp_sock, &remote_rcp_addr, &remote_rtp_addr, loss_pct);
			}
			frame_count = 0;
		}


    }

    if (rcp_timer) { SetThreadpoolTimer(rcp_timer, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(rcp_timer, TRUE); CloseThreadpoolTimer(rcp_timer); }
    if (rtp_timer)  { SetThreadpoolTimer(rtp_timer, NULL, 0, 0); WaitForThreadpoolTimerCallbacks(rtp_timer, TRUE); CloseThreadpoolTimer(rtp_timer); }

    closesocket(rcp_sock);
    closesocket(rtp_sock);
    shutdown_tts();
    WSACleanup();
    timeEndPeriod(1);
    printf("[DONE] HyteraParrot session completed cleanly.\n");
    return 0;
}
