#include "reimpl/window.h"

void *ANativeWindow_fromSurface_soloader(void *env, void *surface) {
    (void)env;
    (void)surface;
    return (void *)1;
}

void ANativeWindow_release_soloader(void *window) {
    (void)window;
}

int ANativeWindow_setBuffersGeometry_soloader(void *window, int width, int height,
                                              int format) {
    (void)window;
    (void)width;
    (void)height;
    (void)format;
    return 0;
}
