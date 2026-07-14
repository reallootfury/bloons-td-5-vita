#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

#include "music.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTD5_MUSIC_MAGIC 0x42544d50u
#define BTD5_MUSIC_GRAIN 1152

typedef struct BTD5MusicPlayer {
    atomic_bool allocated;
    uint32_t magic;
    char path[512];
    atomic_int volume;
    atomic_bool looping;
    atomic_bool unloaded;
} BTD5MusicPlayer;

#define BTD5_MUSIC_HANDLE_COUNT 16
static BTD5MusicPlayer music_handles[BTD5_MUSIC_HANDLE_COUNT];
static _Atomic(BTD5MusicPlayer *) active_player = ATOMIC_VAR_INIT(NULL);
static atomic_bool active_playing = ATOMIC_VAR_INIT(false);
static atomic_uint command_generation = ATOMIC_VAR_INIT(1);
static atomic_bool thread_started = ATOMIC_VAR_INIT(false);

static bool music_handle_valid(BTD5MusicPlayer *player) {
    return player && player->magic == BTD5_MUSIC_MAGIC &&
           !atomic_load_explicit(&player->unloaded, memory_order_acquire);
}

static void close_decoder(mp3dec_ex_t *decoder, bool *decoder_open, int *port) {
    if (*port >= 0) {
        sceAudioOutOutput(*port, NULL);
        sceAudioOutReleasePort(*port);
        *port = -1;
    }
    if (*decoder_open) {
        mp3dec_ex_close(decoder);
        *decoder_open = false;
    }
}

static void *music_thread_main(void *unused) {
    (void)unused;
    mp3dec_ex_t decoder;
    memset(&decoder, 0, sizeof(decoder));
    bool decoder_open = false;
    int port = -1;
    unsigned int local_generation = 0;
    BTD5MusicPlayer *current = NULL;
    mp3d_sample_t samples[BTD5_MUSIC_GRAIN * 2];

    for (;;) {
        unsigned int generation = atomic_load_explicit(
            &command_generation, memory_order_acquire);
        BTD5MusicPlayer *requested = atomic_load_explicit(
            &active_player, memory_order_acquire);

        if (generation != local_generation || requested != current) {
            close_decoder(&decoder, &decoder_open, &port);
            current = requested;
            local_generation = generation;

            if (music_handle_valid(current)) {
                int result = mp3dec_ex_open(&decoder, current->path,
                                            MP3D_SEEK_TO_SAMPLE);
                if (result) {
                    l_error("Music decoder could not open %s (minimp3 %d).",
                            current->path, result);
                    atomic_store_explicit(&active_playing, false,
                                          memory_order_release);
                    log_flush();
                    continue;
                }
                decoder_open = true;

                if (decoder.info.channels < 1 || decoder.info.channels > 2) {
                    l_error("Unsupported music channel count %d for %s.",
                            decoder.info.channels, current->path);
                    close_decoder(&decoder, &decoder_open, &port);
                    atomic_store_explicit(&active_playing, false,
                                          memory_order_release);
                    log_flush();
                    continue;
                }

                SceAudioOutMode mode = decoder.info.channels == 1
                    ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
                port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                           BTD5_MUSIC_GRAIN,
                                           decoder.info.hz, mode);
                if (port < 0) {
                    l_error("Music audio port failed for %s: 0x%08x.",
                            current->path, (unsigned int)port);
                    close_decoder(&decoder, &decoder_open, &port);
                    atomic_store_explicit(&active_playing, false,
                                          memory_order_release);
                    log_flush();
                    continue;
                }

                l_success("Music ready: %s (%d Hz, %d channel%s).",
                          current->path, decoder.info.hz,
                          decoder.info.channels,
                          decoder.info.channels == 1 ? "" : "s");
                log_flush();
            }
        }

        if (!decoder_open || port < 0 || !music_handle_valid(current) ||
            !atomic_load_explicit(&active_playing, memory_order_acquire)) {
            sceKernelDelayThread(10 * 1000);
            continue;
        }

        size_t wanted = BTD5_MUSIC_GRAIN * decoder.info.channels;
        size_t decoded = mp3dec_ex_read(&decoder, samples, wanted);
        if (decoded < wanted &&
            atomic_load_explicit(&current->looping, memory_order_acquire)) {
            if (mp3dec_ex_seek(&decoder, 0) == 0) {
                decoded += mp3dec_ex_read(&decoder, samples + decoded,
                                          wanted - decoded);
            }
        }

        if (decoded < wanted) {
            memset(samples + decoded, 0,
                   (wanted - decoded) * sizeof(samples[0]));
            atomic_store_explicit(&active_playing, false,
                                  memory_order_release);
        }

        if (local_generation != atomic_load_explicit(
                &command_generation, memory_order_acquire)) {
            continue;
        }

        int level = atomic_load_explicit(&current->volume,
                                         memory_order_relaxed);
        int volumes[2] = {level, level};
        sceAudioOutSetVolume(port,
            SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, volumes);
        int output_result = sceAudioOutOutput(port, samples);
        if (output_result < 0) {
            l_error("Music output failed: 0x%08x.",
                    (unsigned int)output_result);
            atomic_store_explicit(&active_playing, false,
                                  memory_order_release);
            log_flush();
        }
    }

    return NULL;
}

static bool ensure_music_thread(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &thread_started, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return true;
    }

    pthread_t thread;
    int result = pthread_create(&thread, NULL, music_thread_main, NULL);
    if (result != 0) {
        atomic_store_explicit(&thread_started, false, memory_order_release);
        l_error("Could not start music thread: %d.", result);
        log_flush();
        return false;
    }
    pthread_detach(thread);
    l_info("Music decoder thread started.");
    log_flush();
    return true;
}

void *btd5_music_load(const char *asset_name) {
    if (!asset_name || !asset_name[0]) {
        l_warn("loadMusic received an empty asset name.");
        return NULL;
    }

    BTD5MusicPlayer *player = NULL;
    for (size_t i = 0; i < BTD5_MUSIC_HANDLE_COUNT; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &music_handles[i].allocated, &expected, true,
                memory_order_acq_rel, memory_order_acquire)) {
            player = &music_handles[i];
            break;
        }
    }
    if (!player) {
        l_error("No free music handles while loading %s.", asset_name);
        return NULL;
    }

    int length = snprintf(player->path, sizeof(player->path),
                          DATA_PATH "assets/%s", asset_name);
    if (length < 0 || (size_t)length >= sizeof(player->path) ||
        !file_exists(player->path)) {
        l_error("Missing music asset: %s", player->path);
        atomic_store_explicit(&player->allocated, false,
                              memory_order_release);
        log_flush();
        return NULL;
    }

    player->magic = BTD5_MUSIC_MAGIC;
    atomic_init(&player->volume, SCE_AUDIO_OUT_MAX_VOL);
    atomic_init(&player->looping, true);
    atomic_init(&player->unloaded, false);
    l_info("Loaded music handle for %s.", player->path);
    return player;
}

void btd5_music_play(void *handle, bool looping) {
    BTD5MusicPlayer *player = handle;
    if (!music_handle_valid(player) || !ensure_music_thread()) {
        return;
    }

    atomic_store_explicit(&player->looping, looping, memory_order_release);
    BTD5MusicPlayer *previous = atomic_exchange_explicit(
        &active_player, player, memory_order_acq_rel);
    if (previous != player) {
        atomic_fetch_add_explicit(&command_generation, 1,
                                  memory_order_acq_rel);
    }
    atomic_store_explicit(&active_playing, true, memory_order_release);
}

void btd5_music_pause(void *handle) {
    BTD5MusicPlayer *player = handle;
    if (music_handle_valid(player) && atomic_load_explicit(
            &active_player, memory_order_acquire) == player) {
        atomic_store_explicit(&active_playing, false, memory_order_release);
    }
}

void btd5_music_set_volume(void *handle, float volume) {
    BTD5MusicPlayer *player = handle;
    if (!music_handle_valid(player)) {
        return;
    }
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    atomic_store_explicit(&player->volume,
        (int)(volume * (float)SCE_AUDIO_OUT_MAX_VOL), memory_order_release);
}

void btd5_music_unload(void *handle) {
    BTD5MusicPlayer *player = handle;
    if (!music_handle_valid(player)) {
        return;
    }
    atomic_store_explicit(&player->unloaded, true, memory_order_release);
    BTD5MusicPlayer *expected = player;
    if (atomic_compare_exchange_strong_explicit(
            &active_player, &expected, NULL,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&active_playing, false, memory_order_release);
        atomic_fetch_add_explicit(&command_generation, 1,
                                  memory_order_acq_rel);
    }
    /* Native references may outlive unloadMusic. Keep the small handle as a
     * tombstone so a late Java-style call cannot become a use-after-free. */
}

bool btd5_music_owns_handle(const void *handle) {
    for (size_t i = 0; i < BTD5_MUSIC_HANDLE_COUNT; i++) {
        if (handle == &music_handles[i]) {
            return true;
        }
    }
    return false;
}
