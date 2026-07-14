#ifndef BTD5_WINDOW_H
#define BTD5_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BTD5 only needs an opaque native window for its EGL setup.  VitaGL owns the
 * actual display surface, so no Android ANativeWindow is ever dereferenced.
 */
void *ANativeWindow_fromSurface_soloader(void *env, void *surface);
void ANativeWindow_release_soloader(void *window);
int ANativeWindow_setBuffersGeometry_soloader(void *window, int width, int height,
                                              int format);

#ifdef __cplusplus
}
#endif

#endif
