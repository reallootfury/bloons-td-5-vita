#ifndef BTD5_VITA_MUSIC_H
#define BTD5_VITA_MUSIC_H

#include <stdbool.h>

void *btd5_music_load(const char *asset_name);
void btd5_music_play(void *handle, bool looping);
void btd5_music_pause(void *handle);
void btd5_music_set_volume(void *handle, float volume);
void btd5_music_unload(void *handle);
bool btd5_music_owns_handle(const void *handle);

#endif
