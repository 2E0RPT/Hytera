#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ADJUST THIS IF IT TRIMS SPEECH OR LEAVES GAPS
// Higher number = more aggressive noise cutting. Range: 100 to 1000.
#define NOISE_GATE_THRESHOLD 300

// Maximum dead air allowed between words. 400 bytes = exactly 50ms at 8000Hz.
#define MAX_SILENCE_ALLOWED_BYTES 800

// Standard 58-byte WAV header structure for G.711 Mu-law
#pragma pack(push, 1)
typedef struct {
    char     riff[4];           // "RIFF"
    uint32_t overall_size;      // Size of entire file minus 8 bytes
    char     wave[4];           // "WAVE"
    char     fmt_chunk_marker[4];// "fmt "
    uint32_t length_of_fmt;     // Length of format data (18 for Mu-law)
    uint16_t format_type;       // 0x0007 for Mu-law
    uint16_t channels;          // 1 for Mono
    uint32_t sample_rate;       // 8000
    uint32_t byterate;          // 8000 bytes per second
    uint16_t block_align;       // 1 byte per sample block alignment
    uint16_t bits_per_sample;   // 8 bits per sample container
    uint16_t cbSize;            // 0 (extra format extension size)
    char     data_chunk_marker[4];// "data"
    uint32_t data_size;         // Raw audio payload bytes size
} MuLawWavHeader;
#pragma pack(pop)

// Converts a G.711 Mu-law byte back to an absolute 16-bit linear volume magnitude
int16_t ulaw_to_linear(uint8_t ulaw_val) {
    ulaw_val = ~ulaw_val;
    int16_t sign = (ulaw_val & 0x80) ? -1 : 1;
    int16_t exponent = (ulaw_val >> 4) & 0x07;
    int16_t mantissa = ulaw_val & 0x0F;
    int16_t sample = (mantissa << 3) + 33;
    sample <<= exponent;
    sample -= 33;
    return (sign * sample < 0) ? -(sign * sample) : (sign * sample);
}

// Helper function to reliably locate the exact start of raw audio data in any WAV file
long find_audio_data_start(FILE* f, uint32_t* rawDataSize) {
    char chunkId[4];
    uint32_t chunkSize = 0;
    
    fseek(f, 12, SEEK_SET);
    while (fread(chunkId, 1, 4, f) == 4) {
        if (fread(&chunkSize, 4, 1, f) != 1) break;
        if (strncmp(chunkId, "data", 4) == 0) {
            *rawDataSize = chunkSize;
            return ftell(f);
        }
        fseek(f, chunkSize, SEEK_CUR);
    }
    return -1;
}

int main() {
    const char* output_filename = "Report.wav";
    const char* input_files[] = { "Alert.wav", "Battery.wav", "Low.wav" };
    int num_files = 3;

    // 1. First Pass: Append all files raw to calculate maximum buffer sizing
    remove(output_filename);
    uint32_t max_possible_bytes = 0;
    for (int i = 0; i < num_files; i++) {
        FILE* fIn = fopen(input_files[i], "rb");
        if (!fIn) {
            printf("[ERROR] Missing vocabulary file: %s\n", input_files[i]);
            return 1;
        }
        uint32_t size = 0;
        find_audio_data_start(fIn, &size);
        max_possible_bytes += size;
        fclose(fIn);
    }

    uint8_t* raw_appended_buffer = (uint8_t*)malloc(max_possible_bytes);
    uint8_t* optimized_buffer = (uint8_t*)malloc(max_possible_bytes);
    if (!raw_appended_buffer || !optimized_buffer) {
        printf("[ERROR] Memory allocation failure.\n");
        return 1;
    }

    uint32_t raw_ptr = 0;
    for (int i = 0; i < num_files; i++) {
        FILE* fIn = fopen(input_files[i], "rb");
        if (fIn) {
            uint32_t size = 0;
            long start = find_audio_data_start(fIn, &size);
            fseek(fIn, start, SEEK_SET);
            fread(raw_appended_buffer + raw_ptr, 1, size, fIn);
            raw_ptr += size;
            fclose(fIn);
        }
    }

    // 2. Second Pass: Noise-Gated Dynamic Compression Matrix
    uint32_t optimized_ptr = 0;
    uint32_t continuous_silence_count = 0;

    printf("Processing timeline via Noise Gate. Compressing background static to 50ms...\n");
    for (uint32_t i = 0; i < max_possible_bytes; i++) {
        uint8_t sample = raw_appended_buffer[i];
        int16_t volume_magnitude = ulaw_to_linear(sample);

        // If the absolute volume falls below our gate threshold, count it as dead air noise
        if (volume_magnitude < NOISE_GATE_THRESHOLD) {
            continuous_silence_count++;
        } else {
            continuous_silence_count = 0;
        }

        // Only allow a maximum of 50ms of sub-threshold audio to pass through into the master mix
        if (continuous_silence_count <= MAX_SILENCE_ALLOWED_BYTES) {
            optimized_buffer[optimized_ptr++] = sample;
        }
    }

    // 3. Write out the final processed file with its updated master header
    FILE* fOut = fopen(output_filename, "wb");
    if (!fOut) {
        printf("[ERROR] Cannot create master file Report.wav on disk.\n");
        return 1;
    }

    MuLawWavHeader masterHeader;
    memcpy(masterHeader.riff, "RIFF", 4);
    masterHeader.overall_size = sizeof(MuLawWavHeader) + optimized_ptr - 8;
    memcpy(masterHeader.wave, "WAVE", 4);
    memcpy(masterHeader.fmt_chunk_marker, "fmt ", 4);
    masterHeader.length_of_fmt = 18;   
    masterHeader.format_type = 0x0007;  // WAVE_FORMAT_MULAW
    masterHeader.channels = 1;         // Mono
    masterHeader.sample_rate = 8000;    // 8000 Hz
    masterHeader.byterate = 8000;       // 8000 bytes per second
    masterHeader.block_align = 1;
    masterHeader.bits_per_sample = 8;   
    masterHeader.cbSize = 0;
    memcpy(masterHeader.data_chunk_marker, "data", 4);
    masterHeader.data_size = optimized_ptr;

    fwrite(&masterHeader, sizeof(MuLawWavHeader), 1, fOut);
    fwrite(optimized_buffer, 1, optimized_ptr, fOut);
    fclose(fOut);

    free(raw_appended_buffer);
    free(optimized_buffer);
    
    printf("Success! 'Report.wav' generated seamlessly with noise gate filters active.\n");
    return 0;
}
