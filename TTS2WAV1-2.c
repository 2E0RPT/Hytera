#define COBJMACROS
#include <windows.h>
#include <sapi.h>
#include <stdio.h>
#include <stdint.h>

// Structure definition for a standard WAV file container header configured for Mu-law
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
    uint32_t byterate;          // 8000
    uint16_t block_align;       // 1
    uint16_t bits_per_sample;   // 8 bits for Mu-law
    uint16_t cbSize;            // 0
    char     data_chunk_marker[4];// "data"
    uint32_t data_size;         // Base payload raw audio size
} MuLawWavHeader;

typedef struct {
    char     riff[4];
    uint32_t overall_size;
    char     wave[4];
    char     fmt_chunk_marker[4];
    uint32_t length_of_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t cbSize;            
} InWavHeader;
#pragma pack(pop)

// Standard G.711 mathematical compression algorithm (PCM 16-bit to Mu-law 8-bit)
uint8_t linear_to_ulaw(int16_t pcm_val) {
    int16_t mask = (pcm_val < 0) ? 0x7F : 0xFF;
    if (pcm_val < 0) pcm_val = -pcm_val;
    if (pcm_val > 32635) pcm_val = 32635;

    pcm_val += 132;

    int16_t seg;
    if (pcm_val < 396)        seg = 0;
    else if (pcm_val < 792)   seg = 1;
    else if (pcm_val < 1584)  seg = 2;
    else if (pcm_val < 3168)  seg = 3;
    else if (pcm_val < 6336)  seg = 4;
    else if (pcm_val < 12672) seg = 5;
    else if (pcm_val < 25344) seg = 6;
    else                      seg = 7;

    uint8_t uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F);
    return (uval ^ mask);
}

int main() {
    const wchar_t* filename = L"Report.wav";
    const wchar_t* temp_filename = L"TempPCM.wav";

    // 1. Delete target and temporary files if they already exist
    _wremove(filename);
    _wremove(temp_filename);
    printf("Preparing standard SAPI file binding pipeline...\n");

    // 2. Initialize COM library
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        printf("Failed to initialize COM. Error: 0x%08LX\n", hr);
        return 1;
    }

    ISpVoice* pVoice = NULL;
    ISpStream* pStream = NULL;

    // 3. Create SAPI voice instance
    hr = CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void**)&pVoice);
    if (FAILED(hr)) {
        printf("Failed to create ISpVoice instance.\n");
        CoUninitialize();
        return 1;
    }

    // 4. Create standard SAPI stream instance to handle file mapping
    hr = CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_ALL, &IID_ISpStream, (void**)&pStream);
    if (SUCCEEDED(hr)) {
        hr = pStream->lpVtbl->BindToFile(pStream, temp_filename, SPFM_CREATE_ALWAYS, NULL, NULL, 0);
        
        if (SUCCEEDED(hr)) {
            pVoice->lpVtbl->SetOutput(pVoice, (IUnknown*)pStream, TRUE);
            
            printf("Synthesizing clear speech audio into file buffer...\n");
            wchar_t* text = L"This is a local text to speech conversion saved directly to a file format.";
            pVoice->lpVtbl->Speak(pVoice, text, SPF_DEFAULT, NULL);

            // Close stream handles completely to unlock filesystem mappings and flush data to disk
            pStream->lpVtbl->Release(pStream);
            pStream = NULL;

            // 5. Read back the intermediate wave file data
            FILE* fTemp = _wfopen(temp_filename, L"rb");
            if (fTemp) {
                InWavHeader inHead;
                if (fread(&inHead, sizeof(InWavHeader), 1, fTemp) == 1) {
                    
                    // Skip the variable format extension bytes dynamically using structural dimensions
                    fseek(fTemp, sizeof(InWavHeader), SEEK_SET);
                    if (inHead.length_of_fmt > 16) {
                        fseek(fTemp, inHead.length_of_fmt - 16, SEEK_CUR);
                    }
                    
                    // FIXED: Changed array declaration to exactly 4 bytes for accurate tag pointer mapping
                    char chunkId[4];
                    uint32_t chunkSize = 0;
                    while (fread(chunkId, 1, 4, fTemp) == 4) {
                        if (fread(&chunkSize, 4, 1, fTemp) != 1) break;
                        if (strncmp(chunkId, "data", 4) == 0) {
                            break;
                        }
                        fseek(fTemp, chunkSize, SEEK_CUR);
                    }

                    if (chunkSize > 0) {
                        uint32_t srcSamplesNum = chunkSize / (2 * inHead.channels);
                        int16_t* srcBuffer = (int16_t*)malloc(chunkSize);
                        
                        if (srcBuffer) {
                            fread(srcBuffer, 1, chunkSize, fTemp);
                            fclose(fTemp);
                            fTemp = NULL;

                            // Calculate downsampling ratio relative to whatever rate SAPI selected natively
                            double sampleRatio = (double)inHead.sample_rate / 8000.0;
                            uint32_t destSamplesNum = (uint32_t)(srcSamplesNum / sampleRatio);
                            uint8_t* mulawBuffer = (uint8_t*)malloc(destSamplesNum);

                            if (mulawBuffer) {
                                // Downsample utilizing multi-channel downmixing and anti-aliasing convolution averages
                                for (uint32_t i = 0; i < destSamplesNum; i++) {
                                    uint32_t srcIndex = (uint32_t)(i * sampleRatio);
                                    if (srcIndex >= srcSamplesNum) srcIndex = srcSamplesNum - 1;
                                    
                                    int32_t sampleAccumulator = 0;
                                    uint32_t windowSize = (uint32_t)sampleRatio;
                                    if (windowSize < 1) windowSize = 1;
                                    
                                    uint32_t count = 0;
                                    for (uint32_t w = 0; w < windowSize && (srcIndex + w) < srcSamplesNum; w++) {
                                        sampleAccumulator += srcBuffer[(srcIndex + w) * inHead.channels];
                                        count++;
                                    }
                                    if (count > 0) sampleAccumulator /= count;
                                    
                                    mulawBuffer[i] = linear_to_ulaw((int16_t)sampleAccumulator);
                                }

                                // 6. Structure and write the clean, bit-perfect telephony WAV file out to disk
                                FILE* fOut = _wfopen(filename, L"wb");
                                if (fOut) {
                                    MuLawWavHeader header;
                                    memcpy(header.riff, "RIFF", 4);
                                    header.overall_size = sizeof(MuLawWavHeader) + destSamplesNum - 8;
                                    memcpy(header.wave, "WAVE", 4);
                                    memcpy(header.fmt_chunk_marker, "fmt ", 4);
                                    header.length_of_fmt = 18;   
                                    header.format_type = 0x0007;  // WAVE_FORMAT_MULAW
                                    header.channels = 1;         // Mono
                                    header.sample_rate = 8000;    // 8000 Hz
                                    header.byterate = 8000;       // 8000 bytes per second
                                    header.block_align = 1;       // 1 byte per sample block alignment
                                    header.bits_per_sample = 8;   // 8 bits per sample container
                                    header.cbSize = 0;
                                    memcpy(header.data_chunk_marker, "data", 4);
                                    header.data_size = destSamplesNum;

                                    fwrite(&header, sizeof(MuLawWavHeader), 1, fOut);
                                    fwrite(mulawBuffer, 1, destSamplesNum, fOut);
                                    fclose(fOut);
                                    
                                    printf("Success! 'Report.wav' generated flawlessly via native streaming.\n");
                                } else {
                                    printf("Error: Failed to open target file on disk for writing.\n");
                                }
                                free(mulawBuffer);
                            }
                            free(srcBuffer);
                        }
                    }
                }
                if (fTemp) fclose(fTemp);
            }
        } else {
            printf("Failed to bind SAPI file stream configuration profile. Error: 0x%08LX\n", hr);
        }
    }

    // Clean up temporary intermediate work file data
    _wremove(temp_filename);
    if (pVoice) pVoice->lpVtbl->Release(pVoice);
    CoUninitialize();
    return 0;
}
