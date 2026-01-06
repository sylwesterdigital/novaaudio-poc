// src/main.c

// Make miniaudio symbols private to this TU to avoid any collision with raylib's bundled miniaudio.
#define MA_API static
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "sonic.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

typedef struct {
    int16_t* pcm;         // interleaved s16 stereo
    uint64_t frames;      // number of frames
    uint32_t channels;    // 2
    uint32_t sampleRate;  // 48000
} BufferS16;

static void buffer_free(BufferS16* b)
{
    if (b->pcm) free(b->pcm);
    memset(b, 0, sizeof(*b));
}

// Improved version that handles format conversion better
static int load_to_s16_stereo48k(const char* path, BufferS16* out)
{
    memset(out, 0, sizeof(*out));

    // First, try to open the file with default settings to get its format
    ma_decoder dec;
    ma_result r = ma_decoder_init_file(path, NULL, &dec);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "ma_decoder_init_file failed (%d) for: %s\n", (int)r, path);
        return 0;
    }

    ma_format srcFormat = dec.outputFormat;
    ma_uint32 srcChannels = dec.outputChannels;
    ma_uint32 srcSampleRate = dec.outputSampleRate;
    
    fprintf(stderr, "File format: format=%d, channels=%u, sampleRate=%u\n",
            (int)srcFormat, srcChannels, srcSampleRate);
    
    // Reinitialize with our desired format if needed
    if (srcFormat != ma_format_s16 || srcChannels != 2 || srcSampleRate != 48000) {
        ma_decoder_uninit(&dec);
        
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 2, 48000);
        r = ma_decoder_init_file(path, &cfg, &dec);
        if (r != MA_SUCCESS) {
            fprintf(stderr, "Failed to reinit decoder with target format (%d)\n", (int)r);
            return 0;
        }
    }

    const ma_uint64 chunkFrames = 4096;
    int16_t* tmp = (int16_t*)malloc((size_t)chunkFrames * 2 * sizeof(int16_t));
    if (!tmp) {
        ma_decoder_uninit(&dec);
        return 0;
    }

    int16_t* pcm = NULL;
    size_t capFrames = 0;
    size_t usedFrames = 0;

    for (;;) {
        ma_uint64 framesRead = 0;
        r = ma_decoder_read_pcm_frames(&dec, tmp, chunkFrames, &framesRead);
        if (r != MA_SUCCESS) {
            if (r == MA_AT_END) {
                // Reached end of file - this is expected
                break;
            }
            fprintf(stderr, "ma_decoder_read_pcm_frames failed (%d) for: %s\n", (int)r, path);
            free(pcm);
            free(tmp);
            ma_decoder_uninit(&dec);
            return 0;
        }
        if (framesRead == 0) {
            break; // No more data
        }

        if (usedFrames + (size_t)framesRead > capFrames) {
            size_t newCap = capFrames ? capFrames * 2 : (size_t)framesRead * 8;
            while (newCap < usedFrames + (size_t)framesRead) newCap *= 2;

            int16_t* newPcm = (int16_t*)realloc(pcm, newCap * 2 * sizeof(int16_t));
            if (!newPcm) {
                free(pcm);
                free(tmp);
                ma_decoder_uninit(&dec);
                return 0;
            }
            pcm = newPcm;
            capFrames = newCap;
        }

        memcpy(pcm + usedFrames * 2, tmp, (size_t)framesRead * 2 * sizeof(int16_t));
        usedFrames += (size_t)framesRead;
    }

    free(tmp);
    ma_decoder_uninit(&dec);

    if (usedFrames == 0) {
        free(pcm);
        fprintf(stderr, "Decoded 0 frames for: %s\n", path);
        return 0;
    }

    out->pcm = pcm;
    out->frames = (uint64_t)usedFrames;
    out->channels = 2;
    out->sampleRate = 48000;

    fprintf(stderr, "Loaded OK: %s | frames=%llu | sr=48000 | ch=2\n",
            path, (unsigned long long)out->frames);

    return 1;
}

// ---------------- Engine ----------------
typedef struct {
    ma_device dev;
    BufferS16 buf;
    sonicStream st;

    atomic_int playing;
    atomic_int reverse;
    _Atomic float tempo;   // 0.5 .. 2.0
    _Atomic float volume;  // 0 .. 1
    atomic_int loop;

    double cursor; // frame index
} Engine;

static Engine g;

static uint32_t read_from_buffer(Engine* e, int16_t* out, uint32_t outFrames)
{
    if (!e->buf.pcm || e->buf.frames == 0) return 0;

    const int rev  = atomic_load(&e->reverse);
    const int loop = atomic_load(&e->loop);

    for (uint32_t i = 0; i < outFrames; i++) {
        if (!rev) {
            if (e->cursor >= (double)(e->buf.frames - 1)) {
                if (loop) e->cursor = 0.0;
                else return i;
            }
        } else {
            if (e->cursor <= 0.0) {
                if (loop) e->cursor = (double)(e->buf.frames - 1);
                else return i;
            }
        }

        uint64_t idx = (uint64_t)e->cursor;
        const int16_t* p = e->buf.pcm + idx * 2;
        out[i*2 + 0] = p[0];
        out[i*2 + 1] = p[1];

        e->cursor += rev ? -1.0 : 1.0;
    }

    return outFrames;
}

static void audio_cb(ma_device* d, void* outp, const void* inp, ma_uint32 frameCount)
{
    (void)inp;
    Engine* e = (Engine*)d->pUserData;
    int16_t* out = (int16_t*)outp;

    if (!e || atomic_load(&e->playing) == 0 || e->buf.pcm == NULL) {
        memset(out, 0, (size_t)frameCount * 2 * sizeof(int16_t));
        return;
    }

    int16_t dry[2048 * 2];
    uint32_t want = (frameCount > 2048) ? 2048 : (uint32_t)frameCount;

    uint32_t got = read_from_buffer(e, dry, want);
    if (got == 0) {
        memset(out, 0, (size_t)frameCount * 2 * sizeof(int16_t));
        atomic_store(&e->playing, 0);
        return;
    }

    float tempo = atomic_load(&e->tempo);
    if (tempo < 0.1f) tempo = 0.1f;
    sonicSetSpeed(e->st, tempo);

    float vol = atomic_load(&e->volume);
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    sonicSetVolume(e->st, vol);

    sonicWriteShortToStream(e->st, dry, (int)got);

    uint32_t written = 0;
    while (written < (uint32_t)frameCount) {
        int need = (int)((uint32_t)frameCount - written);
        int gotOut = sonicReadShortFromStream(e->st, out + written * 2, need);
        if (gotOut <= 0) break;
        written += (uint32_t)gotOut;
    }

    if (written < (uint32_t)frameCount) {
        memset(out + written * 2, 0, ((uint32_t)frameCount - written) * 2 * sizeof(int16_t));
    }
}

static int engine_load(Engine* e, const char* path)
{
    atomic_store(&e->playing, 0);
    buffer_free(&e->buf);
    
    fprintf(stderr, "Attempting to load: %s\n", path);
    
    if (!load_to_s16_stereo48k(path, &e->buf)) {
        fprintf(stderr, "Failed to load file\n");
        return 0;
    }
    
    fprintf(stderr, "Loaded %llu frames\n", (unsigned long long)e->buf.frames);
    
    e->cursor = 0.0;

    if (e->st) sonicDestroyStream(e->st);
    e->st = sonicCreateStream(48000, 2);
    if (!e->st) {
        fprintf(stderr, "Failed to create sonic stream\n");
        return 0;
    }
    sonicSetQuality(e->st, 1);
    
    fprintf(stderr, "Engine load successful\n");
    return 1;
}

int main(int argc, char** argv)
{
    const char* path = (argc >= 2) ? argv[1] : NULL;

    InitWindow(980, 560, "novaaudio-poc");
    SetTargetFPS(60);

    memset(&g, 0, sizeof(g));
    atomic_store(&g.playing, 0);
    atomic_store(&g.reverse, 0);
    atomic_store(&g.loop, 1);
    atomic_store(&g.tempo, 1.0f);
    atomic_store(&g.volume, 1.0f);

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format   = ma_format_s16;
    dc.playback.channels = 2;
    dc.sampleRate        = 48000;
    dc.dataCallback      = audio_cb;
    dc.pUserData         = &g;

    if (ma_device_init(NULL, &dc, &g.dev) != MA_SUCCESS) {
        fprintf(stderr, "ma_device_init failed\n");
        return 2;
    }
    if (ma_device_start(&g.dev) != MA_SUCCESS) {
        fprintf(stderr, "ma_device_start failed\n");
        ma_device_uninit(&g.dev);
        return 3;
    }

    char currentFile[1024] = {0};
    // At the start of main, before engine_load
    if (path) {
        FILE* test = fopen(path, "rb");
        if (!test) {
            fprintf(stderr, "Cannot open file: %s\n", path);
        } else {
            fclose(test);
            strncpy(currentFile, path, sizeof(currentFile)-1);
            if (engine_load(&g, currentFile)) atomic_store(&g.playing, 1);
        }
    }

    while (!WindowShouldClose()) {
        if (IsFileDropped()) {
            FilePathList files = LoadDroppedFiles();
            if (files.count > 0) {
                strncpy(currentFile, files.paths[0], sizeof(currentFile)-1);
                if (engine_load(&g, currentFile)) atomic_store(&g.playing, 1);
            }
            UnloadDroppedFiles(files);
        }

        if (IsKeyPressed(KEY_SPACE)) atomic_store(&g.playing, atomic_load(&g.playing) ? 0 : 1);
        if (IsKeyPressed(KEY_R))     atomic_store(&g.reverse, atomic_load(&g.reverse) ? 0 : 1);

        BeginDrawing();
        ClearBackground((Color){18,18,22,255});

        DrawText("Drop WAV/MP3. SPACE: play/pause | R: reverse", 20, 18, 18, RAYWHITE);
        DrawText(currentFile[0] ? currentFile : "(no file loaded)", 20, 46, 14, (Color){200,200,210,255});

        Rectangle panel = (Rectangle){20, 90, 420, 430};
        GuiPanel(panel, "Controls");

        int playing = atomic_load(&g.playing);
        int reverse = atomic_load(&g.reverse);

        if (GuiButton((Rectangle){40, 130, 160, 32}, playing ? "Pause" : "Play")) {
            atomic_store(&g.playing, playing ? 0 : 1);
        }
        if (GuiButton((Rectangle){220, 130, 200, 32}, reverse ? "Reverse: ON" : "Reverse: OFF")) {
            atomic_store(&g.reverse, reverse ? 0 : 1);
        }
        if (GuiButton((Rectangle){40, 170, 160, 32}, "Rewind")) {
            g.cursor = reverse ? (double)(g.buf.frames ? (g.buf.frames - 1) : 0) : 0.0;
            if (g.st) sonicFlushStream(g.st);
        }

        bool loop = atomic_load(&g.loop) != 0;
        GuiCheckBox((Rectangle){220, 178, 18, 18}, "Loop", &loop);
        atomic_store(&g.loop, loop ? 1 : 0);

        DrawText("Tempo (no pitch change)", 40, 230, 14, RAYWHITE);
        float tempoUI = atomic_load(&g.tempo);
        GuiSlider((Rectangle){40, 250, 380, 18}, "0.5x", "2.0x", &tempoUI, 0.5f, 2.0f);
        atomic_store(&g.tempo, tempoUI);

        DrawText("Volume", 40, 290, 14, RAYWHITE);
        float volUI = atomic_load(&g.volume);
        GuiSlider((Rectangle){40, 310, 380, 18}, "0", "1", &volUI, 0.0f, 1.0f);
        atomic_store(&g.volume, volUI);

        EndDrawing();
    }

    atomic_store(&g.playing, 0);
    if (g.st) sonicDestroyStream(g.st);
    buffer_free(&g.buf);

    ma_device_uninit(&g.dev);
    CloseWindow();
    return 0;
}
