#include "reimpl/asset_manager.h"
#include "utils/logger.h"
#include "game.h"

#include <pthread.h>
#include <malloc.h>
#include <cstring>
#include <cstdio>
#include <libc_bridge/libc_bridge.h>
#include <string>
#include <fcntl.h>
#include <dirent.h>

typedef struct assetManager {
    int dummy = 0; // TODO: mb we will need to store something here in future
    pthread_mutex_t mLock;
} assetManager;

typedef struct aAsset {
    char * filename;
    FILE* f;
    size_t bytesRead;
    size_t fileSize;
    bool opened = false;
} asset;

typedef struct aAssetDir {
    DIR *dir;
    struct dirent *entry;
} assetDir;

static AAssetManager * g_AAssetManager = nullptr;
static unsigned long long diag_opens;
static unsigned long long diag_active;
static unsigned long long diag_reads;
static unsigned long long diag_bytes;
static unsigned long long diag_seeks;
static unsigned long long diag_last_position;
static unsigned long long diag_last_size;
static bool diag_eof_short_circuit_logged;

static FILE *open_asset_file(const std::string &path) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fopen(path.c_str(), "rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

AAssetManager * AAssetManager_create() {
    if (g_AAssetManager) return g_AAssetManager;

    assetManager am;

    pthread_mutex_init(&am.mLock, nullptr);

    g_AAssetManager = (AAssetManager *) malloc(sizeof(assetManager));
    memcpy(g_AAssetManager, &am, sizeof(assetManager));

    return g_AAssetManager;
}

AAssetManager * AAssetManager_fromJava(void *env, void *assetManager) {
    (void)env;
    (void)assetManager;
    return AAssetManager_create();
}

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
    (void)mgr;
    (void)mode;
    if (!filename) {
        return nullptr;
    }

    std::string realp = std::string(btd5_data_path()) + "assets/" + filename;
    FILE *file = open_asset_file(realp);
    if (!file) {
        return nullptr;
    }

    auto * a = new aAsset;
    a->filename = (char *) malloc(realp.length() + 1);
    strcpy(a->filename, realp.c_str());
    a->bytesRead = 0;
    a->f = file;
    {
#ifdef USE_SCELIBC_IO
        sceLibcBridge_fseek(a->f, 0, SEEK_END);
        a->fileSize = sceLibcBridge_ftell(a->f);
        sceLibcBridge_fseek(a->f, 0, SEEK_SET);
#else
        fseek(a->f, 0, SEEK_END);
        a->fileSize = ftell(a->f);
        fseek(a->f, 0, SEEK_SET);
#endif
        a->opened = true;
        __atomic_add_fetch(&diag_opens, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&diag_active, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&diag_last_size, a->fileSize, __ATOMIC_RELAXED);
        __atomic_store_n(&diag_last_position, 0, __ATOMIC_RELAXED);
    }

#ifdef DEBUG_ASSET_IO_VERBOSE
    l_debug("AAssetManager_open<%p>(%p, %s, %i): %p", __builtin_return_address(0), mgr, realp.c_str(), mode, a);
#endif
    return (AAsset *) a;
}

void AAsset_close(AAsset* asset) {
#ifdef DEBUG_ASSET_IO_VERBOSE
    l_debug("AAsset_close<%p>(%p)", __builtin_return_address(0), asset);
#endif

    if (asset) {
        auto * a = (aAsset *) asset;
        free(a->filename);
        if (a->opened) {
#ifdef USE_SCELIBC_IO
            sceLibcBridge_fclose(a->f);
#else
            fclose(a->f);
#endif
        }
        if (a->opened) {
            __atomic_sub_fetch(&diag_active, 1, __ATOMIC_RELAXED);
        }
        delete a;
    }
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
#ifdef DEBUG_ASSET_IO_VERBOSE
    l_debug("AAsset_read<%p>(%p, %p, %i)", __builtin_return_address(0), asset, buf, count);
#endif

    if (!asset) {
        return -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

    /* SceLibcBridge/FIOS can leave a synchronous fread waiting on its
     * completion semaphore when it is submitted with the stream already at
     * exact EOF. Android AAsset_read instead guarantees an immediate zero at
     * EOF. BTD5's byte-oriented archive parser performs one final read after
     * consuming the last byte, so enforce the Android contract before
     * entering the bridge and never submit a request beyond the asset. */
    if (count == 0) {
        return 0;
    }
    if (a->bytesRead >= a->fileSize) {
        if (!__atomic_exchange_n(&diag_eof_short_circuit_logged, true,
                                 __ATOMIC_RELAXED)) {
            l_info("AAsset EOF short-circuit: %s (%u/%u).", a->filename,
                   (unsigned int)a->bytesRead, (unsigned int)a->fileSize);
        }
        return 0;
    }
    size_t remaining = a->fileSize - a->bytesRead;
    if (count > remaining) {
        count = remaining;
    }

#ifdef USE_SCELIBC_IO
    size_t ret = sceLibcBridge_fread(buf, 1, count, a->f);
#else
    size_t ret = fread(buf, 1, count, a->f);
#endif

    if (ret > 0) {
        a->bytesRead += ret;
        __atomic_add_fetch(&diag_reads, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&diag_bytes, ret, __ATOMIC_RELAXED);
        __atomic_store_n(&diag_last_position, a->bytesRead, __ATOMIC_RELAXED);
        __atomic_store_n(&diag_last_size, a->fileSize, __ATOMIC_RELAXED);
        return (int) ret;
    } else {
#ifdef USE_SCELIBC_IO
        if (sceLibcBridge_feof(a->f)) {
#else
        if (feof(a->f)) {
#endif
            return 0;
        } else {
            return -1;
        }
    }
}

off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
#ifdef DEBUG_ASSET_IO_VERBOSE
    l_debug("AAsset_seek(%p, %d, %i)", asset, offset, whence);
#endif

    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

#ifdef USE_SCELIBC_IO
    auto ret = (off_t) sceLibcBridge_fseek(a->f, offset, whence);
#else
    auto ret = (off_t) fseek(a->f, offset, whence);
#endif
    if (ret != 0) return (off_t)-1;

#ifdef USE_SCELIBC_IO
    a->bytesRead = (size_t)sceLibcBridge_ftell(a->f);
#else
    a->bytesRead = (size_t)ftell(a->f);
#endif
    __atomic_add_fetch(&diag_seeks, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&diag_last_position, a->bytesRead, __ATOMIC_RELAXED);
    __atomic_store_n(&diag_last_size, a->fileSize, __ATOMIC_RELAXED);
    return (off_t)a->bytesRead;
}

void AAsset_getDiagnostics(AAssetDiagnostics *diagnostics) {
    if (!diagnostics) return;
    diagnostics->opens = __atomic_load_n(&diag_opens, __ATOMIC_RELAXED);
    diagnostics->active = __atomic_load_n(&diag_active, __ATOMIC_RELAXED);
    diagnostics->reads = __atomic_load_n(&diag_reads, __ATOMIC_RELAXED);
    diagnostics->bytes = __atomic_load_n(&diag_bytes, __ATOMIC_RELAXED);
    diagnostics->seeks = __atomic_load_n(&diag_seeks, __ATOMIC_RELAXED);
    diagnostics->last_position = __atomic_load_n(&diag_last_position,
                                                  __ATOMIC_RELAXED);
    diagnostics->last_size = __atomic_load_n(&diag_last_size,
                                              __ATOMIC_RELAXED);
}

off_t AAsset_getRemainingLength(AAsset* asset) {
#ifdef DEBUG_ASSET_IO_VERBOSE
    l_debug("AAsset_getRemainingLength");
#endif
    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    if (!a->opened) {
        return -1;
    }

    return (off_t)(a->fileSize - a->bytesRead);
}

off_t AAsset_getLength(AAsset* asset) {
#ifdef DEBUG_ASSET_IO_VERBOSE
    l_debug("AAsset_getLength");
#endif
    if (!asset) {
        return (off_t) -1;
    }

    auto * a = (aAsset *) asset;

    return (off_t)a->fileSize;
}

AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName) {
    (void)mgr;
    std::string realp = std::string(btd5_data_path()) + "assets/" + (dirName ? dirName : "");
    auto *dir = new aAssetDir;
    dir->dir = opendir(realp.c_str());
    dir->entry = nullptr;
    if (!dir->dir) {
        delete dir;
        l_warn("AAssetManager_openDir: cannot open %s", realp.c_str());
        return nullptr;
    }
    return (AAssetDir *)dir;
}

const char* AAssetDir_getNextFileName(AAssetDir* assetDir) {
    if (!assetDir) return nullptr;
    auto *dir = (aAssetDir *)assetDir;
    while ((dir->entry = readdir(dir->dir)) != nullptr) {
        const char *name = dir->entry->d_name;
        if (strcmp(name, ".") && strcmp(name, "..")) return name;
    }
    return nullptr;
}

void AAssetDir_close(AAssetDir* assetDir) {
    if (!assetDir) return;
    auto *dir = (aAssetDir *)assetDir;
    if (dir->dir) closedir(dir->dir);
    delete dir;
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (!asset) {
        l_warn("AAsset_openFileDescriptor(%p, %p, %p): asset is null", asset, outStart, outLength);
        return -1;
    }
    auto * a = (aAsset *) asset;
    if (outStart) *outStart = 0;
    if (outLength) *outLength = a->fileSize;
    if (a->opened) {
        if (a->opened) {
#ifdef USE_SCELIBC_IO
            sceLibcBridge_fclose(a->f);
#else
            fclose(a->f);
#endif
        }
        a->opened = false;
        __atomic_sub_fetch(&diag_active, 1, __ATOMIC_RELAXED);
    }
    int ret = open(a->filename, O_RDONLY);
    l_debug("AAsset_openFileDescriptor(%p/\"%s\", %p, %p): ret %i", asset, a->filename, outStart, outLength, ret);
    return ret;
}
