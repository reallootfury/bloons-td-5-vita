/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

void glCompileShader_soloader(GLuint shader);

void glLinkProgram_soloader(GLuint program);

void glFinish_soloader(void);

void glFlush_soloader(void);


void gl_state_cache_invalidate(void);

void glActiveTexture_soloader(GLenum texture);
void glBindTexture_soloader(GLenum target, GLuint texture);
void glBindBuffer_soloader(GLenum target, GLuint buffer);
void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer);
void glBindRenderbuffer_soloader(GLenum target, GLuint renderbuffer);
void glUseProgram_soloader(GLuint program);
void glEnable_soloader(GLenum capability);
void glDisable_soloader(GLenum capability);
void glEnableVertexAttribArray_soloader(GLuint index);
void glDisableVertexAttribArray_soloader(GLuint index);
void glVertexAttribPointer_soloader(GLuint index, GLint size, GLenum type,
                                    GLboolean normalized, GLsizei stride,
                                    const void *pointer);
void glBlendFunc_soloader(GLenum source, GLenum destination);
void glBlendFuncSeparate_soloader(GLenum source_rgb, GLenum destination_rgb,
                                  GLenum source_alpha,
                                  GLenum destination_alpha);
void glBlendEquation_soloader(GLenum mode);
void glBlendEquationSeparate_soloader(GLenum mode_rgb, GLenum mode_alpha);
void glColorMask_soloader(GLboolean red, GLboolean green, GLboolean blue,
                          GLboolean alpha);
void glDepthMask_soloader(GLboolean flag);
void glDepthFunc_soloader(GLenum function);
void glCullFace_soloader(GLenum mode);
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glClearColor_soloader(GLfloat red, GLfloat green, GLfloat blue,
                           GLfloat alpha);
void glDeleteTextures_soloader(GLsizei count, const GLuint *textures);
void glDeleteBuffers_soloader(GLsizei count, const GLuint *buffers);
void glDeleteProgram_soloader(GLuint program);
void glDeleteFramebuffers_soloader(GLsizei count, const GLuint *framebuffers);
void glDeleteRenderbuffers_soloader(GLsizei count,
                                    const GLuint *renderbuffers);

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const void *indices);

typedef struct GLDrawStats {
    unsigned long long array_calls;
    unsigned long long element_calls;
    unsigned long long array_vertices;
    unsigned long long element_indices;
    unsigned long long draw_cpu_us;
    unsigned long long draw_cpu_max_us;
    unsigned long long draw_over_1ms;
    unsigned long long draw_over_4ms;
    unsigned long long state_calls;
    unsigned long long state_skipped;
    bool detailed_timing;
} GLDrawStats;

void gl_take_draw_stats(GLDrawStats *stats);

typedef struct GLProfileSample {
    unsigned long long draw_calls;
    unsigned long long draw_vertices;
    unsigned long long draw_cpu_us;
    unsigned long long draw_cpu_max_us;
} GLProfileSample;

void gl_profile_sample_begin(void);
void gl_profile_sample_end(GLProfileSample *sample);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
