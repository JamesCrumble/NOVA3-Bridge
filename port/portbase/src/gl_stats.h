/*
 * GL pipeline census - measurement only, no behaviour change.
 *
 * See gl_stats.cpp for why this exists.
 */
#ifndef PORTBASE_GL_STATS_H
#define PORTBASE_GL_STATS_H

#include "khronos/glad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cheap enough to sit on the draw path: one relaxed load of a cached int. */
int gl_stats_enabled(void);
int gl_stats_error_check_enabled(void);

/* Called once the context is current and both GL tables are filled, so the
 * report can name which table answered each entry point. */
void gl_stats_init(void);

/* State the wrappers observe on the way past. None of these touch GL. */
void gl_stats_note_use_program(GLuint program);
void gl_stats_note_bind_buffer(GLenum target, GLuint buffer);
void gl_stats_note_buffer_data(GLenum target, long size, GLenum usage,
                               int has_data);
void gl_stats_note_vertex_attrib_pointer(GLuint index, GLint size, GLenum type,
                                         GLboolean normalized, GLsizei stride,
                                         const void *pointer);
void gl_stats_note_vertex_attrib_array(GLuint index, int enabled);
void gl_stats_note_bind_texture(GLenum target, GLuint texture);
void gl_stats_note_bind_framebuffer(GLenum target, GLuint framebuffer);
void gl_stats_note_framebuffer_attach(const char *entry, GLenum attachment,
                                      GLuint name);
void gl_stats_note_capability(GLenum cap, int enabled);
void gl_stats_note_viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void gl_stats_note_scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void gl_stats_note_clear(GLbitfield mask);
void gl_stats_note_draw(GLenum mode, GLsizei count, int indexed, GLenum type);
void gl_stats_note_texture_upload(int compressed, GLenum internalformat,
                                  GLsizei width, GLsizei height, long bytes,
                                  int forwarded);

/* The pipeline state the census has been shadowing, for code that has to
 * recognise one specific draw without a per-call glGet round trip (which under
 * qemu costs more than the draw). Any pointer may be NULL. */
void gl_stats_current_state(GLuint *program, GLuint *framebuffer,
                            int *blend, int *depth_test);

/* A wrapper that decided not to call the driver. Screams once per symbol. */
void gl_stats_note_dropped(const char *symbol, const char *why);

/* glGetError drain, attributed to `what`. No-op unless error checking is on. */
void gl_stats_check_error(const char *what);

/* Frame boundary; dumps every <PREFIX>_GL_STATS frames. */
void gl_stats_frame(long frame);

/* Force a dump tagged with a label, so a screenshot and a counter window can
 * be lined up without counting log lines. */
void gl_stats_mark(const char *label, long frame);

#ifdef __cplusplus
}
#endif

#endif /* PORTBASE_GL_STATS_H */
