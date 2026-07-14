/*
 * vitaGL's optional animated splashscreen renders from a second GXM context.
 * On BTD5's first console run that worker faulted inside SceGxm while the game
 * thread waited for it to release the renderer.  Supplying the four symbols
 * used by vitaGL keeps splashscreen.o out of the static link, which is the
 * runtime equivalent of building vitaGL with NO_SPLASHSCREEN=1.
 */

#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

SceUID splash_mutex[2];
GLboolean is_splashscreen_active = GL_FALSE;

void invoke_splashscreen(void) {
}

void clear_splashscreen(void) {
}
