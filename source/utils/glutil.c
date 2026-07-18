/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "game.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>

#include <so_util/so_util.h>

extern so_module so_mod;

static bool gl_initialized = false;
static unsigned int shader_compile_count;
static unsigned int program_link_count;
static unsigned int gl_finish_count;
static unsigned int gl_flush_count;
static unsigned long long gl_array_draw_calls;
static unsigned long long gl_element_draw_calls;
static unsigned long long gl_array_vertices;
static unsigned long long gl_element_indices;

static unsigned int game_address_offset(const void *address) {
    uintptr_t value = (uintptr_t)address;
    if (value >= so_mod.text_base &&
        value < so_mod.text_base + so_mod.text_size) {
        return (unsigned int)(value - so_mod.text_base);
    }
    return 0xffffffffu;
}

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

    if (settings_low_graphics_applied()) {
        /* Keep the display at its known-good native mode. Let ShaRK optimize
         * the translated shaders more aggressively instead of relying on an
         * unsupported intermediate framebuffer size. */
        vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, SHARK_ENABLE,
                                      SHARK_ENABLE, SHARK_ENABLE);
        l_info("Low Graphics: native 960x544 with fast shader optimization.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    if (gl_initialized)
        return;

    // BTD5 is a 2D title and its EGL config reports no multisampling. Avoiding
    // 4x MSAA also leaves substantially more graphics memory for game assets.
    int width = settings_render_width();
    int height = settings_render_height();
    l_info("Initializing VitaGL at %dx%d (Low Graphics %s).", width, height,
           settings_low_graphics_applied() ? "ON" : "OFF");
    vglInitExtended(0, width, height, 6 * 1024 * 1024,
                    SCE_GXM_MULTISAMPLE_NONE);
    gl_initialized = true;
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    ++gl_array_draw_calls;
    if (count > 0) {
        gl_array_vertices += (unsigned long long)count;
    }
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const void *indices) {
    ++gl_element_draw_calls;
    if (count > 0) {
        gl_element_indices += (unsigned long long)count;
    }
    glDrawElements(mode, count, type, indices);
}

void gl_take_draw_stats(GLDrawStats *stats) {
    if (!stats) {
        return;
    }
    stats->array_calls = gl_array_draw_calls;
    stats->element_calls = gl_element_draw_calls;
    stats->array_vertices = gl_array_vertices;
    stats->element_indices = gl_element_indices;
    gl_array_draw_calls = 0;
    gl_element_draw_calls = 0;
    gl_array_vertices = 0;
    gl_element_indices = 0;
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        unsigned int sequence = ++shader_compile_count;
        uint64_t compile_start = sceKernelGetProcessTimeWide();
#ifdef DUMP_COMPILED_SHADERS
        l_info("Compiling uncached shader: %s", next_shader_fname);
#else
        l_info("Compiling GLSL shader #%u from SO+0x%08x.", sequence,
               game_address_offset(__builtin_return_address(0)));
#endif
        glCompileShader(shader);
        uint64_t compile_ms =
            (sceKernelGetProcessTimeWide() - compile_start) / 1000ULL;
        l_info("Shader compile finished in %llu ms.",
               (unsigned long long)compile_ms);
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len = 0;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        if (bin && len > 0 && next_shader_fname[0]) {
            file_mkpath(next_shader_fname, 0777);
            file_save(next_shader_fname, bin, (size_t)len);
        } else {
            l_warn("VitaGL returned no shader binary; cache entry skipped.");
            if (next_shader_fname[0]) {
                remove(next_shader_fname);
            }
        }
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

void glLinkProgram_soloader(GLuint program) {
    unsigned int sequence = ++program_link_count;
    l_warn("Entering glLinkProgram #%u (program %u) from SO+0x%08x.",
           sequence, program,
           game_address_offset(__builtin_return_address(0)));
    log_flush();
    glLinkProgram(program);
    l_warn("glLinkProgram #%u returned.", sequence);
    log_flush();
}

void glFinish_soloader(void) {
    unsigned int sequence = ++gl_finish_count;
    l_warn("Entering glFinish #%u from SO+0x%08x.", sequence,
           game_address_offset(__builtin_return_address(0)));
    log_flush();
    glFinish();
    l_warn("glFinish #%u returned.", sequence);
    log_flush();
}

void glFlush_soloader(void) {
    unsigned int sequence = ++gl_flush_count;
    l_warn("Entering glFlush #%u from SO+0x%08x.", sequence,
           game_address_offset(__builtin_return_address(0)));
    log_flush();
    glFlush();
    l_warn("glFlush #%u returned.", sequence);
    log_flush();
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), "%sgxp/%s.gxp", btd5_data_path(), sha_name);

    bool cache_loaded = false;
    if (file_exists(gxp_path)) {
        uint8_t *buffer = NULL;
        size_t size = 0;

        if (file_load(gxp_path, &buffer, &size) && buffer && size > 0) {
            glShaderBinary(1, &shader, 0, buffer, (int32_t) size);
            cache_loaded = true;
            skip_next_compile = GL_TRUE;
        } else {
            l_warn("Discarding invalid shader cache entry: %s", gxp_path);
            remove(gxp_path);
        }
        free(buffer);
    }

    if (!cache_loaded) {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), "%sgxp/%s.gxp", btd5_data_path(), sha_name);
    snprintf(cg_path, sizeof(cg_path), "%scg/%s.cg", btd5_data_path(), sha_name);

    bool cache_loaded = false;
    if (file_exists(gxp_path)) {
        uint8_t *buffer = NULL;
        size_t size = 0;

        if (file_load(gxp_path, &buffer, &size) && buffer && size > 0) {
            glShaderBinary(1, &shader, 0, buffer, (int32_t) size);
            cache_loaded = true;
            skip_next_compile = GL_TRUE;
        } else {
            l_warn("Discarding invalid shader cache entry: %s", gxp_path);
            remove(gxp_path);
        }
        free(buffer);
    }

    if (!cache_loaded && file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else if (!cache_loaded) {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), "%sglsl/%s.glsl", btd5_data_path(), sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), "%scg/%s.cg", btd5_data_path(), sha_name);
#else
    snprintf(path, sizeof(path), "%sgxp/%s.gxp", btd5_data_path(), sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), "%sglsl/%s.glsl", btd5_data_path(), sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
