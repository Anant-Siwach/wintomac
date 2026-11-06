// Shared between runtime/opengl.c (WGL + hand-written GL entry points) and the generated runtime/gen/gl_thunks.c.
#pragma once
#include "win32.h"

void (*gl_host_proc(const char *name))(void);  // host function from OpenGL.framework (or NULL)
void gl_missing(const char *name);             // log a call to a GL function macOS lacks (rate-limited)
