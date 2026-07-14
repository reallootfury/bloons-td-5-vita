/*
 * OpenSLES' Vita backend references libsndfile only for URI players. BTD5
 * uses Android simple-buffer queues, so linking a hard-float libsndfile is
 * unnecessary. Keep those optional URI entry points unavailable.
 */
#include <stdint.h>

typedef struct SNDFILE_tag SNDFILE;
typedef struct SF_INFO SF_INFO;
typedef int64_t sf_count_t;

SNDFILE *sf_open(const char *path, int mode, SF_INFO *info) {
    (void)path; (void)mode; (void)info;
    return 0;
}

int sf_close(SNDFILE *sndfile) {
    (void)sndfile;
    return 0;
}

sf_count_t sf_read_short(SNDFILE *sndfile, short *ptr, sf_count_t items) {
    (void)sndfile; (void)ptr; (void)items;
    return 0;
}

sf_count_t sf_seek(SNDFILE *sndfile, sf_count_t frames, int whence) {
    (void)sndfile; (void)frames; (void)whence;
    return -1;
}
