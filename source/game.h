#ifndef BTD5_GAME_H
#define BTD5_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BTD5GameVersion {
    BTD5_VERSION_337 = 337,
    BTD5_VERSION_47 = 407,
} BTD5GameVersion;

void btd5_select_game_version(void);
int btd5_verify_native_fingerprint(uint32_t *actual_crc,
                                   uint64_t *actual_size);
uint32_t btd5_expected_native_crc(void);
uint64_t btd5_expected_native_size(void);
BTD5GameVersion btd5_game_version(void);
const char *btd5_game_version_name(void);
const char *btd5_data_path(void);
const char *btd5_so_path(void);
void btd5_path(char *output, size_t output_size, const char *suffix);

#ifdef __cplusplus
}
#endif

#endif
