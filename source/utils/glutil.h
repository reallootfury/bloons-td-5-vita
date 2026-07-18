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

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const void *indices);

typedef struct GLDrawStats {
    unsigned long long array_calls;
    unsigned long long element_calls;
    unsigned long long array_vertices;
    unsigned long long element_indices;
} GLDrawStats;

void gl_take_draw_stats(GLDrawStats *stats);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
