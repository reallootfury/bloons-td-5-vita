#include "game.h"

#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <zlib.h>

#define BOOT_CHOICE_PATH DATA_PATH ".boot-version"
#define BTD5_337_NATIVE_SIZE UINT64_C(9851888)
#define BTD5_337_NATIVE_CRC  UINT32_C(0x37afbb66)
#define BTD5_47_NATIVE_SIZE  UINT64_C(8683424)
#define BTD5_47_NATIVE_CRC   UINT32_C(0x53bea993)

static BTD5GameVersion selected_version = BTD5_VERSION_337;
static const char *selected_data_path = DATA_PATH "3.37/";
static char selected_so_path[128] = DATA_PATH "3.37/libnative.so";

static int consume_boot_choice(void) {
    char choice[4] = {0};
    SceUID fd = sceIoOpen(BOOT_CHOICE_PATH, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;

    int bytes_read = sceIoRead(fd, choice, sizeof(choice) - 1);
    sceIoClose(fd);
    sceIoRemove(BOOT_CHOICE_PATH);
    if (bytes_read <= 0)
        return 0;
    if (choice[0] == '4')
        return 407;
    if (choice[0] == '3')
        return 337;
    return 0;
}

static void relaunch_with_boot_choice(int version) {
    const char *choice = version == 407 ? "4.7" : "3.37";
    SceUID fd = sceIoOpen(BOOT_CHOICE_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0 || sceIoWrite(fd, choice, 3) != 3) {
        if (fd >= 0)
            sceIoClose(fd);
        fatal_error("Could not save the BTD5 boot selection.");
    }
    sceIoClose(fd);

    l_info("Saved one-shot BTD5 %s selection; relaunching with clean memory.",
           choice);
    log_flush();
    int result = sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL);
    sceIoRemove(BOOT_CHOICE_PATH);
    fatal_error("Could not relaunch BTD5 after version selection (0x%08X).",
                result);
}

void btd5_select_game_version(void) {
    const char *v337_path = DATA_PATH "3.37/";
    const char *v47_path = DATA_PATH "4.7/";
    char v337_so[128];
    char v337_assets[160];
    char v47_so[128];
    char v47_assets[160];
    snprintf(v337_so, sizeof(v337_so), "%slibnative.so", v337_path);
    snprintf(v337_assets, sizeof(v337_assets),
             "%sassets/Assets/BTD5.jet", v337_path);
    snprintf(v47_so, sizeof(v47_so), "%slibnative.so", v47_path);
    snprintf(v47_assets, sizeof(v47_assets),
             "%sassets/Assets/BTD5.jet", v47_path);

    bool v337_ready = file_exists(v337_so) && file_exists(v337_assets);
    bool v47_ready = file_exists(v47_so) && file_exists(v47_assets);

    int requested_version = consume_boot_choice();
    if (v337_ready && v47_ready) {
        if (requested_version == 337 || requested_version == 407) {
            l_info("Consumed one-shot BTD5 %s boot selection.",
                   requested_version == 407 ? "4.7" : "3.37");
        } else {
            l_info("Both complete BTD5 data sets detected; opening version picker.");
            log_flush();
            requested_version = select_btd5_version_dialog();
            if (requested_version == 0) {
                l_info("Version selection cancelled.");
                log_flush();
                sceKernelExitProcess(0);
            }
            /* The picker needs VitaGL, whose allocations conflict with the
             * fixed-address Android image. Relaunch so so_file_load starts in
             * a clean process, then consume the one-shot selection above. */
            relaunch_with_boot_choice(requested_version);
        }
    } else if (v47_ready) {
        requested_version = 407;
        l_info("Only the complete BTD5 4.7 data set was detected; booting it automatically.");
    } else if (v337_ready) {
        requested_version = 337;
        l_info("Only the complete BTD5 3.37 data set was detected; booting it automatically.");
    } else {
        requested_version = 337;
        l_warn("No complete BTD5 data set detected; using 3.37 paths for the missing-file report.");
    }

    if (requested_version == 407) {
        selected_version = BTD5_VERSION_47;
        selected_data_path = v47_path;
        snprintf(selected_so_path, sizeof(selected_so_path), "%s", v47_so);
    } else {
        selected_version = BTD5_VERSION_337;
        selected_data_path = v337_path;
        snprintf(selected_so_path, sizeof(selected_so_path), "%s", v337_so);
    }

    l_info("Selected BTD5 %s data at %s.", btd5_game_version_name(),
           selected_data_path);
}

uint32_t btd5_expected_native_crc(void) {
    return selected_version == BTD5_VERSION_47
        ? BTD5_47_NATIVE_CRC : BTD5_337_NATIVE_CRC;
}

uint64_t btd5_expected_native_size(void) {
    return selected_version == BTD5_VERSION_47
        ? BTD5_47_NATIVE_SIZE : BTD5_337_NATIVE_SIZE;
}

int btd5_verify_native_fingerprint(uint32_t *actual_crc,
                                   uint64_t *actual_size) {
    SceUID fd = sceIoOpen(selected_so_path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }

    unsigned char buffer[32 * 1024];
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t size = 0;
    int read_result;
    while ((read_result = sceIoRead(fd, buffer, sizeof(buffer))) > 0) {
        crc = crc32(crc, buffer, (uInt)read_result);
        size += (uint64_t)read_result;
    }
    sceIoClose(fd);
    if (read_result < 0) {
        return read_result;
    }

    if (actual_crc) {
        *actual_crc = (uint32_t)crc;
    }
    if (actual_size) {
        *actual_size = size;
    }
    return size == btd5_expected_native_size() &&
           (uint32_t)crc == btd5_expected_native_crc();
}

BTD5GameVersion btd5_game_version(void) { return selected_version; }

const char *btd5_game_version_name(void) {
    return selected_version == BTD5_VERSION_47 ? "4.7" : "3.37";
}

const char *btd5_data_path(void) { return selected_data_path; }
const char *btd5_so_path(void) { return selected_so_path; }

void btd5_path(char *output, size_t output_size, const char *suffix) {
    snprintf(output, output_size, "%s%s", selected_data_path,
             suffix ? suffix : "");
}
