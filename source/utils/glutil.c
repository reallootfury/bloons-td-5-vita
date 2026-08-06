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

/* Some VitaGL header revisions omit these GLES 2.0 core capability enums.
 * Keep the Android state-cache bridge source-compatible with those revisions. */
#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#ifndef GL_SAMPLE_ALPHA_TO_COVERAGE
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#endif
#ifndef GL_SAMPLE_COVERAGE
#define GL_SAMPLE_COVERAGE 0x80A0
#endif

extern so_module so_mod;

static bool gl_initialized = false;
static bool gl_diagnostics_enabled = false;
static unsigned int shader_compile_count;
static unsigned int program_link_count;
static unsigned int gl_finish_count;
static unsigned int gl_flush_count;
static unsigned long long gl_array_draw_calls;
static unsigned long long gl_element_draw_calls;
static unsigned long long gl_array_vertices;
static unsigned long long gl_element_indices;
static unsigned long long gl_draw_cpu_us;
static unsigned long long gl_draw_cpu_max_us;
static unsigned long long gl_draw_over_1ms;
static unsigned long long gl_draw_over_4ms;
static unsigned long long gl_state_calls;
static unsigned long long gl_state_skipped;

#ifdef BTD5_NATIVE_PHASE_PROFILER
static bool gl_profile_active;
static GLProfileSample gl_profile_sample;
#endif

#define GL_STATE_TEXTURE_UNITS 16U
#define GL_STATE_VERTEX_ATTRIBS 16U
#define GL_STATE_CAPS 9U

typedef struct GLTextureUnitState {
    bool valid_2d;
    bool valid_cube;
    GLuint texture_2d;
    GLuint texture_cube;
} GLTextureUnitState;

typedef struct GLVertexAttribPointerState {
    bool valid;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const void *pointer;
    GLuint array_buffer;
} GLVertexAttribPointerState;

typedef struct GLStateCache {
    bool active_texture_valid;
    GLenum active_texture;
    bool program_valid;
    GLuint program;
    bool array_buffer_valid;
    bool element_buffer_valid;
    GLuint array_buffer;
    GLuint element_buffer;
    bool framebuffer_valid;
    bool renderbuffer_valid;
    GLuint framebuffer;
    GLuint renderbuffer;
    bool cap_valid[GL_STATE_CAPS];
    bool cap_enabled[GL_STATE_CAPS];
    bool attrib_valid[GL_STATE_VERTEX_ATTRIBS];
    bool attrib_enabled[GL_STATE_VERTEX_ATTRIBS];
    GLVertexAttribPointerState attrib_pointer[GL_STATE_VERTEX_ATTRIBS];
    bool blend_func_valid;
    GLenum blend_src_rgb;
    GLenum blend_dst_rgb;
    GLenum blend_src_alpha;
    GLenum blend_dst_alpha;
    bool blend_equation_valid;
    GLenum blend_equation_rgb;
    GLenum blend_equation_alpha;
    bool color_mask_valid;
    GLboolean color_mask[4];
    bool depth_mask_valid;
    GLboolean depth_mask;
    bool depth_func_valid;
    GLenum depth_func;
    bool cull_face_valid;
    GLenum cull_face;
    bool viewport_valid;
    GLint viewport_x;
    GLint viewport_y;
    GLsizei viewport_width;
    GLsizei viewport_height;
    bool scissor_valid;
    GLint scissor_x;
    GLint scissor_y;
    GLsizei scissor_width;
    GLsizei scissor_height;
    bool clear_color_valid;
    GLfloat clear_color[4];
    GLTextureUnitState textures[GL_STATE_TEXTURE_UNITS];
} GLStateCache;

static GLStateCache gl_state_cache;

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
#if defined(DEBUG_SOLOADER) || defined(BTD5_RELEASE_TELEMETRY)
    gl_diagnostics_enabled = log_is_enabled();
#else
    /* The logging VPK is also used for normal play. Do not make every GL state
     * wrapper and draw update counters merely because file logging is enabled.
     * The state cache itself remains active; only telemetry bookkeeping is off. */
    gl_diagnostics_enabled = false;
#endif
    l_info("Visual compatibility v5: native 960x544 with original High texture/XML pairs; no Low asset redirect or low-precision shader substitution.");
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
        l_info("Compatibility performance mode: native 960x544, original High atlas assets, fast shader compilation; no texture-set or resolution substitution.");
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


void gl_state_cache_invalidate(void) {
    memset(&gl_state_cache, 0, sizeof(gl_state_cache));
}

static bool gl_state_skip(bool same) {
    if (gl_diagnostics_enabled) {
        ++gl_state_calls;
        if (same) {
            ++gl_state_skipped;
        }
    }
    return same;
}

static int gl_capability_slot(GLenum capability) {
    switch (capability) {
        case GL_BLEND: return 0;
        case GL_CULL_FACE: return 1;
        case GL_DEPTH_TEST: return 2;
        case GL_DITHER: return 3;
        case GL_POLYGON_OFFSET_FILL: return 4;
        case GL_SCISSOR_TEST: return 5;
        case GL_STENCIL_TEST: return 6;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: return 7;
        case GL_SAMPLE_COVERAGE: return 8;
        default: return -1;
    }
}

static int gl_active_texture_slot(void) {
    if (!gl_state_cache.active_texture_valid ||
        gl_state_cache.active_texture < GL_TEXTURE0) {
        return -1;
    }
    unsigned int unit =
        (unsigned int)(gl_state_cache.active_texture - GL_TEXTURE0);
    return unit < GL_STATE_TEXTURE_UNITS ? (int)unit : -1;
}

void glActiveTexture_soloader(GLenum texture) {
    bool same = gl_state_cache.active_texture_valid &&
                gl_state_cache.active_texture == texture;
    if (gl_state_skip(same)) return;
    glActiveTexture(texture);
    gl_state_cache.active_texture = texture;
    gl_state_cache.active_texture_valid = true;
}

void glBindTexture_soloader(GLenum target, GLuint texture) {
    bool *valid = NULL;
    GLuint *current = NULL;
    int unit = gl_active_texture_slot();
    if (unit >= 0) {
        GLTextureUnitState *state = &gl_state_cache.textures[unit];
        if (target == GL_TEXTURE_2D) {
            valid = &state->valid_2d;
            current = &state->texture_2d;
        } else if (target == GL_TEXTURE_CUBE_MAP) {
            valid = &state->valid_cube;
            current = &state->texture_cube;
        }
    }
    bool same = valid && *valid && *current == texture;
    if (gl_state_skip(same)) return;
    glBindTexture(target, texture);
    if (valid) {
        *valid = true;
        *current = texture;
    }
}

void glBindBuffer_soloader(GLenum target, GLuint buffer) {
    bool *valid = NULL;
    GLuint *current = NULL;
    if (target == GL_ARRAY_BUFFER) {
        valid = &gl_state_cache.array_buffer_valid;
        current = &gl_state_cache.array_buffer;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        valid = &gl_state_cache.element_buffer_valid;
        current = &gl_state_cache.element_buffer;
    }
    bool same = valid && *valid && *current == buffer;
    if (gl_state_skip(same)) return;
    glBindBuffer(target, buffer);
    if (valid) {
        *valid = true;
        *current = buffer;
    }
}

void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
    bool same = gl_state_cache.framebuffer_valid &&
                gl_state_cache.framebuffer == framebuffer;
    if (gl_state_skip(same)) return;
    glBindFramebuffer(target, framebuffer);
    gl_state_cache.framebuffer = framebuffer;
    gl_state_cache.framebuffer_valid = true;
}

void glBindRenderbuffer_soloader(GLenum target, GLuint renderbuffer) {
    bool same = gl_state_cache.renderbuffer_valid &&
                gl_state_cache.renderbuffer == renderbuffer;
    if (gl_state_skip(same)) return;
    glBindRenderbuffer(target, renderbuffer);
    gl_state_cache.renderbuffer = renderbuffer;
    gl_state_cache.renderbuffer_valid = true;
}

void glUseProgram_soloader(GLuint program) {
    bool same = gl_state_cache.program_valid &&
                gl_state_cache.program == program;
    if (gl_state_skip(same)) return;
    glUseProgram(program);
    gl_state_cache.program = program;
    gl_state_cache.program_valid = true;
}

static void gl_set_capability(GLenum capability, bool enabled) {
    int slot = gl_capability_slot(capability);
    bool same = slot >= 0 && gl_state_cache.cap_valid[slot] &&
                gl_state_cache.cap_enabled[slot] == enabled;
    if (gl_state_skip(same)) return;
    if (enabled) glEnable(capability);
    else glDisable(capability);
    if (slot >= 0) {
        gl_state_cache.cap_valid[slot] = true;
        gl_state_cache.cap_enabled[slot] = enabled;
    }
}

void glEnable_soloader(GLenum capability) {
    gl_set_capability(capability, true);
}

void glDisable_soloader(GLenum capability) {
    gl_set_capability(capability, false);
}

static void gl_set_vertex_attrib(GLuint index, bool enabled) {
    bool same = index < GL_STATE_VERTEX_ATTRIBS &&
                gl_state_cache.attrib_valid[index] &&
                gl_state_cache.attrib_enabled[index] == enabled;
    if (gl_state_skip(same)) return;
    if (enabled) glEnableVertexAttribArray(index);
    else glDisableVertexAttribArray(index);
    if (index < GL_STATE_VERTEX_ATTRIBS) {
        gl_state_cache.attrib_valid[index] = true;
        gl_state_cache.attrib_enabled[index] = enabled;
    }
}

void glEnableVertexAttribArray_soloader(GLuint index) {
    gl_set_vertex_attrib(index, true);
}

void glDisableVertexAttribArray_soloader(GLuint index) {
    gl_set_vertex_attrib(index, false);
}

void glVertexAttribPointer_soloader(GLuint index, GLint size, GLenum type,
                                    GLboolean normalized, GLsizei stride,
                                    const void *pointer) {
    bool cacheable = index < GL_STATE_VERTEX_ATTRIBS &&
                     gl_state_cache.array_buffer_valid;
    GLVertexAttribPointerState *state = cacheable ?
        &gl_state_cache.attrib_pointer[index] : NULL;
    bool same = state && state->valid &&
                state->size == size &&
                state->type == type &&
                state->normalized == normalized &&
                state->stride == stride &&
                state->pointer == pointer &&
                state->array_buffer == gl_state_cache.array_buffer;
    if (gl_state_skip(same)) return;

    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    if (state) {
        state->valid = true;
        state->size = size;
        state->type = type;
        state->normalized = normalized;
        state->stride = stride;
        state->pointer = pointer;
        state->array_buffer = gl_state_cache.array_buffer;
    }
}

void glBlendFuncSeparate_soloader(GLenum source_rgb, GLenum destination_rgb,
                                  GLenum source_alpha,
                                  GLenum destination_alpha) {
    bool same = gl_state_cache.blend_func_valid &&
                gl_state_cache.blend_src_rgb == source_rgb &&
                gl_state_cache.blend_dst_rgb == destination_rgb &&
                gl_state_cache.blend_src_alpha == source_alpha &&
                gl_state_cache.blend_dst_alpha == destination_alpha;
    if (gl_state_skip(same)) return;
    glBlendFuncSeparate(source_rgb, destination_rgb,
                        source_alpha, destination_alpha);
    gl_state_cache.blend_func_valid = true;
    gl_state_cache.blend_src_rgb = source_rgb;
    gl_state_cache.blend_dst_rgb = destination_rgb;
    gl_state_cache.blend_src_alpha = source_alpha;
    gl_state_cache.blend_dst_alpha = destination_alpha;
}

void glBlendFunc_soloader(GLenum source, GLenum destination) {
    bool same = gl_state_cache.blend_func_valid &&
                gl_state_cache.blend_src_rgb == source &&
                gl_state_cache.blend_dst_rgb == destination &&
                gl_state_cache.blend_src_alpha == source &&
                gl_state_cache.blend_dst_alpha == destination;
    if (gl_state_skip(same)) return;
    glBlendFunc(source, destination);
    gl_state_cache.blend_func_valid = true;
    gl_state_cache.blend_src_rgb = source;
    gl_state_cache.blend_dst_rgb = destination;
    gl_state_cache.blend_src_alpha = source;
    gl_state_cache.blend_dst_alpha = destination;
}

void glBlendEquationSeparate_soloader(GLenum mode_rgb, GLenum mode_alpha) {
    bool same = gl_state_cache.blend_equation_valid &&
                gl_state_cache.blend_equation_rgb == mode_rgb &&
                gl_state_cache.blend_equation_alpha == mode_alpha;
    if (gl_state_skip(same)) return;
    glBlendEquationSeparate(mode_rgb, mode_alpha);
    gl_state_cache.blend_equation_valid = true;
    gl_state_cache.blend_equation_rgb = mode_rgb;
    gl_state_cache.blend_equation_alpha = mode_alpha;
}

void glBlendEquation_soloader(GLenum mode) {
    bool same = gl_state_cache.blend_equation_valid &&
                gl_state_cache.blend_equation_rgb == mode &&
                gl_state_cache.blend_equation_alpha == mode;
    if (gl_state_skip(same)) return;
    glBlendEquation(mode);
    gl_state_cache.blend_equation_valid = true;
    gl_state_cache.blend_equation_rgb = mode;
    gl_state_cache.blend_equation_alpha = mode;
}

void glColorMask_soloader(GLboolean red, GLboolean green, GLboolean blue,
                          GLboolean alpha) {
    bool same = gl_state_cache.color_mask_valid &&
                gl_state_cache.color_mask[0] == red &&
                gl_state_cache.color_mask[1] == green &&
                gl_state_cache.color_mask[2] == blue &&
                gl_state_cache.color_mask[3] == alpha;
    if (gl_state_skip(same)) return;
    glColorMask(red, green, blue, alpha);
    gl_state_cache.color_mask_valid = true;
    gl_state_cache.color_mask[0] = red;
    gl_state_cache.color_mask[1] = green;
    gl_state_cache.color_mask[2] = blue;
    gl_state_cache.color_mask[3] = alpha;
}

void glDepthMask_soloader(GLboolean flag) {
    bool same = gl_state_cache.depth_mask_valid &&
                gl_state_cache.depth_mask == flag;
    if (gl_state_skip(same)) return;
    glDepthMask(flag);
    gl_state_cache.depth_mask = flag;
    gl_state_cache.depth_mask_valid = true;
}

void glDepthFunc_soloader(GLenum function) {
    bool same = gl_state_cache.depth_func_valid &&
                gl_state_cache.depth_func == function;
    if (gl_state_skip(same)) return;
    glDepthFunc(function);
    gl_state_cache.depth_func = function;
    gl_state_cache.depth_func_valid = true;
}

void glCullFace_soloader(GLenum mode) {
    bool same = gl_state_cache.cull_face_valid &&
                gl_state_cache.cull_face == mode;
    if (gl_state_skip(same)) return;
    glCullFace(mode);
    gl_state_cache.cull_face = mode;
    gl_state_cache.cull_face_valid = true;
}

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    bool same = gl_state_cache.viewport_valid &&
                gl_state_cache.viewport_x == x &&
                gl_state_cache.viewport_y == y &&
                gl_state_cache.viewport_width == width &&
                gl_state_cache.viewport_height == height;
    if (gl_state_skip(same)) return;
    glViewport(x, y, width, height);
    gl_state_cache.viewport_valid = true;
    gl_state_cache.viewport_x = x;
    gl_state_cache.viewport_y = y;
    gl_state_cache.viewport_width = width;
    gl_state_cache.viewport_height = height;
}

void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    bool same = gl_state_cache.scissor_valid &&
                gl_state_cache.scissor_x == x &&
                gl_state_cache.scissor_y == y &&
                gl_state_cache.scissor_width == width &&
                gl_state_cache.scissor_height == height;
    if (gl_state_skip(same)) return;
    glScissor(x, y, width, height);
    gl_state_cache.scissor_valid = true;
    gl_state_cache.scissor_x = x;
    gl_state_cache.scissor_y = y;
    gl_state_cache.scissor_width = width;
    gl_state_cache.scissor_height = height;
}

void glClearColor_soloader(GLfloat red, GLfloat green, GLfloat blue,
                           GLfloat alpha) {
    bool same = gl_state_cache.clear_color_valid &&
                gl_state_cache.clear_color[0] == red &&
                gl_state_cache.clear_color[1] == green &&
                gl_state_cache.clear_color[2] == blue &&
                gl_state_cache.clear_color[3] == alpha;
    if (gl_state_skip(same)) return;
    glClearColor(red, green, blue, alpha);
    gl_state_cache.clear_color_valid = true;
    gl_state_cache.clear_color[0] = red;
    gl_state_cache.clear_color[1] = green;
    gl_state_cache.clear_color[2] = blue;
    gl_state_cache.clear_color[3] = alpha;
}

void glDeleteTextures_soloader(GLsizei count, const GLuint *textures) {
    glDeleteTextures(count, textures);
    gl_state_cache_invalidate();
}

void glDeleteBuffers_soloader(GLsizei count, const GLuint *buffers) {
    glDeleteBuffers(count, buffers);
    gl_state_cache_invalidate();
}

void glDeleteProgram_soloader(GLuint program) {
    glDeleteProgram(program);
    gl_state_cache_invalidate();
}

void glDeleteFramebuffers_soloader(GLsizei count,
                                   const GLuint *framebuffers) {
    glDeleteFramebuffers(count, framebuffers);
    gl_state_cache_invalidate();
}

void glDeleteRenderbuffers_soloader(GLsizei count,
                                    const GLuint *renderbuffers) {
    glDeleteRenderbuffers(count, renderbuffers);
    gl_state_cache_invalidate();
}

static void gl_record_draw_duration(uint64_t elapsed_us) {
    gl_draw_cpu_us += elapsed_us;
    if (elapsed_us > gl_draw_cpu_max_us) gl_draw_cpu_max_us = elapsed_us;
    if (elapsed_us >= 1000ULL) ++gl_draw_over_1ms;
    if (elapsed_us >= 4000ULL) ++gl_draw_over_4ms;
}

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    if (gl_diagnostics_enabled) {
        ++gl_array_draw_calls;
        if (count > 0) gl_array_vertices += (unsigned long long)count;
    }

    /* Normal Release frames avoid two process-time syscalls per draw.
     * Only DEBUG_SOLOADER or an opt-in sampled profiler frame enables them. */
    bool time_draw = false;
#ifdef DEBUG_SOLOADER
    time_draw = gl_diagnostics_enabled;
#endif
#ifdef BTD5_NATIVE_PHASE_PROFILER
    time_draw = time_draw || gl_profile_active;
#endif
    uint64_t started_us = time_draw ? sceKernelGetProcessTimeWide() : 0;
    glDrawArrays(mode, first, count);
    if (time_draw) {
        uint64_t elapsed_us = sceKernelGetProcessTimeWide() - started_us;
#ifdef DEBUG_SOLOADER
        if (gl_diagnostics_enabled) gl_record_draw_duration(elapsed_us);
#endif
#ifdef BTD5_NATIVE_PHASE_PROFILER
        if (gl_profile_active) {
            ++gl_profile_sample.draw_calls;
            if (count > 0) {
                gl_profile_sample.draw_vertices += (unsigned long long)count;
            }
            gl_profile_sample.draw_cpu_us += elapsed_us;
            if (elapsed_us > gl_profile_sample.draw_cpu_max_us) {
                gl_profile_sample.draw_cpu_max_us = elapsed_us;
            }
        }
#endif
    }
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const void *indices) {
    if (gl_diagnostics_enabled) {
        ++gl_element_draw_calls;
        if (count > 0) gl_element_indices += (unsigned long long)count;
    }

    bool time_draw = false;
#ifdef DEBUG_SOLOADER
    time_draw = gl_diagnostics_enabled;
#endif
#ifdef BTD5_NATIVE_PHASE_PROFILER
    time_draw = time_draw || gl_profile_active;
#endif
    uint64_t started_us = time_draw ? sceKernelGetProcessTimeWide() : 0;
    glDrawElements(mode, count, type, indices);
    if (time_draw) {
        uint64_t elapsed_us = sceKernelGetProcessTimeWide() - started_us;
#ifdef DEBUG_SOLOADER
        if (gl_diagnostics_enabled) gl_record_draw_duration(elapsed_us);
#endif
#ifdef BTD5_NATIVE_PHASE_PROFILER
        if (gl_profile_active) {
            ++gl_profile_sample.draw_calls;
            if (count > 0) {
                gl_profile_sample.draw_vertices += (unsigned long long)count;
            }
            gl_profile_sample.draw_cpu_us += elapsed_us;
            if (elapsed_us > gl_profile_sample.draw_cpu_max_us) {
                gl_profile_sample.draw_cpu_max_us = elapsed_us;
            }
        }
#endif
    }
}

void gl_profile_sample_begin(void) {
#ifdef BTD5_NATIVE_PHASE_PROFILER
    memset(&gl_profile_sample, 0, sizeof(gl_profile_sample));
    gl_profile_active = true;
#endif
}

void gl_profile_sample_end(GLProfileSample *sample) {
    if (!sample) return;
#ifdef BTD5_NATIVE_PHASE_PROFILER
    gl_profile_active = false;
    *sample = gl_profile_sample;
    memset(&gl_profile_sample, 0, sizeof(gl_profile_sample));
#else
    memset(sample, 0, sizeof(*sample));
#endif
}

void gl_take_draw_stats(GLDrawStats *stats) {
    if (!stats) {
        return;
    }
    stats->array_calls = gl_array_draw_calls;
    stats->element_calls = gl_element_draw_calls;
    stats->array_vertices = gl_array_vertices;
    stats->element_indices = gl_element_indices;
    stats->draw_cpu_us = gl_draw_cpu_us;
    stats->draw_cpu_max_us = gl_draw_cpu_max_us;
    stats->draw_over_1ms = gl_draw_over_1ms;
    stats->draw_over_4ms = gl_draw_over_4ms;
    stats->state_calls = gl_state_calls;
    stats->state_skipped = gl_state_skipped;
#ifdef DEBUG_SOLOADER
    stats->detailed_timing = gl_diagnostics_enabled;
#else
    stats->detailed_timing = false;
#endif
    gl_array_draw_calls = 0;
    gl_element_draw_calls = 0;
    gl_array_vertices = 0;
    gl_element_indices = 0;
    gl_draw_cpu_us = 0;
    gl_draw_cpu_max_us = 0;
    gl_draw_over_1ms = 0;
    gl_draw_over_4ms = 0;
    gl_state_calls = 0;
    gl_state_skipped = 0;
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
    if (!log_is_enabled()) {
        glFinish();
        return;
    }
    unsigned int sequence = ++gl_finish_count;
    l_warn("Entering glFinish #%u from SO+0x%08x.", sequence,
           game_address_offset(__builtin_return_address(0)));
    log_flush();
    glFinish();
    l_warn("glFinish #%u returned.", sequence);
    log_flush();
}

void glFlush_soloader(void) {
    if (!log_is_enabled()) {
        glFlush();
        return;
    }
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
        GLint source_length = (GLint)length;
        glShaderSource(shader, 1, &string, &source_length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    GLint source_length = (GLint)length;
    glShaderSource(shader, 1, &string, &source_length);
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
