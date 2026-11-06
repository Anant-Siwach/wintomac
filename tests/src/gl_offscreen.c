// Headless OpenGL: a legacy context and a 3.2 core context, a triangle drawn into a 64x64 framebuffer
// object, read back and hashed. Built twice: as a Windows exe (recompiled; WGL + opengl32 through our
// bridge, on a hidden window) and natively for macOS with CGL (reference). Both end up in Apple's OpenGL
// on the same GPU, so the output must match exactly.
//
// Exercises the Win64 argument paths the generated GL thunks must get right: four floats in xmm0-3
// (glClearColor), an int then floats with the fifth argument a float on the stack (glUniform4f), seven
// integer/pointer arguments with three on the stack (glReadPixels), and pointer returns (glGetString).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#define GLX(T, n) static T p_##n;
#define GLFUNCS \
    GLX(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) GLX(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    GLX(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) GLX(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    GLX(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) GLX(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    GLX(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) GLX(PFNGLCREATESHADERPROC, glCreateShader) \
    GLX(PFNGLSHADERSOURCEPROC, glShaderSource) GLX(PFNGLCOMPILESHADERPROC, glCompileShader) \
    GLX(PFNGLGETSHADERIVPROC, glGetShaderiv) GLX(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    GLX(PFNGLCREATEPROGRAMPROC, glCreateProgram) GLX(PFNGLATTACHSHADERPROC, glAttachShader) \
    GLX(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) GLX(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation) \
    GLX(PFNGLLINKPROGRAMPROC, glLinkProgram) GLX(PFNGLGETPROGRAMIVPROC, glGetProgramiv) GLX(PFNGLUSEPROGRAMPROC, glUseProgram) \
    GLX(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) GLX(PFNGLUNIFORM4FPROC, glUniform4f) \
    GLX(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) GLX(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    GLX(PFNGLGENBUFFERSPROC, glGenBuffers) GLX(PFNGLBINDBUFFERPROC, glBindBuffer) GLX(PFNGLBUFFERDATAPROC, glBufferData) \
    GLX(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) GLX(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    GLX(PFNGLGETSTRINGIPROC, glGetStringi)
GLFUNCS
#undef GLX
#define GL(n) p_##n
#else
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#define GL(n) n
#endif

enum { W = 64, H = 64 };

static const char *vs_src = "#version 150\nin vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src = "#version 150\nuniform vec4 col;\nout vec4 o;\nvoid main() { o = col; }\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = GL(glCreateShader)(type);
    GL(glShaderSource)(s, 1, &src, NULL);
    GL(glCompileShader)(s);
    GLint ok = 0; GL(glGetShaderiv)(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; GL(glGetShaderInfoLog)(s, sizeof log, NULL, log); printf("shader error: %s\n", log); }
    return s;
}

static int draw(void) {
    GLuint fbo, rb;
    GL(glGenFramebuffers)(1, &fbo); GL(glBindFramebuffer)(GL_FRAMEBUFFER, fbo);
    GL(glGenRenderbuffers)(1, &rb); GL(glBindRenderbuffer)(GL_RENDERBUFFER, rb);
    GL(glRenderbufferStorage)(GL_RENDERBUFFER, GL_RGBA8, W, H);
    GL(glFramebufferRenderbuffer)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    printf("framebuffer complete: %s\n", GL(glCheckFramebufferStatus)(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? "yes" : "no");

    GLuint prog = GL(glCreateProgram)();
    GL(glAttachShader)(prog, shader(GL_VERTEX_SHADER, vs_src));
    GL(glAttachShader)(prog, shader(GL_FRAGMENT_SHADER, fs_src));
    GL(glBindAttribLocation)(prog, 0, "pos");
    GL(glBindFragDataLocation)(prog, 0, "o");
    GL(glLinkProgram)(prog);
    GLint ok = 0; GL(glGetProgramiv)(prog, GL_LINK_STATUS, &ok);
    printf("program linked: %s\n", ok ? "yes" : "no");
    GL(glUseProgram)(prog);
    GL(glUniform4f)(GL(glGetUniformLocation)(prog, "col"), 1.0f, 0.5f, 0.25f, 0.75f);

    static const float verts[] = { -0.8f, -0.8f, 0.8f, -0.6f, 0.0f, 0.9f };
    GLuint vao, vbo;
    GL(glGenVertexArrays)(1, &vao); GL(glBindVertexArray)(vao);
    GL(glGenBuffers)(1, &vbo); GL(glBindBuffer)(GL_ARRAY_BUFFER, vbo);
    GL(glBufferData)(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    GL(glEnableVertexAttribArray)(0);

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.25f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    static uint8_t px[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t hsh = 2166136261u; int lit = 0;
    for (int i = 0; i < W * H * 4; i++) hsh = (hsh ^ px[i]) * 16777619u;
    for (int i = 0; i < W * H; i++) lit += px[i * 4] == 255;
    const uint8_t *m = px + ((H / 2) * W + W / 2) * 4, *k = px;
    printf("center %d,%d,%d,%d corner %d,%d,%d,%d\n", m[0], m[1], m[2], m[3], k[0], k[1], k[2], k[3]);
    printf("triangle pixels %d, hash %08x, error %u\n", lit, hsh, glGetError());
    return 0;
}

int main(void) {
#ifdef _WIN32
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "gltest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "gltest", "gl", WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, NULL, NULL, wc.hInstance, NULL);  // never shown
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    int fmt = ChoosePixelFormat(dc, &pfd);
    PIXELFORMATDESCRIPTOR got; int nfmt = DescribePixelFormat(dc, fmt, sizeof got, &got);
    printf("pixel format chosen: %s, formats > 0: %s, depth %d stencil %d\n", fmt > 0 ? "yes" : "no", nfmt > 0 ? "yes" : "no", got.cDepthBits, got.cStencilBits);
    printf("set pixel format: %d\n", SetPixelFormat(dc, fmt, &pfd));
    HGLRC legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    printf("legacy version %.3s\n", (const char *)glGetString(GL_VERSION));
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    PFNWGLGETEXTENSIONSSTRINGARBPROC exts = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");
    printf("WGL_ARB_create_context_profile: %s\n", exts && strstr(exts(dc), "WGL_ARB_create_context_profile") ? "yes" : "no");
    printf("missing function is NULL: %s\n", wglGetProcAddress("glNotARealFunction") == NULL ? "yes" : "no");
    int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 2,
                      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    HGLRC core = create ? create(dc, NULL, attribs) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!core || !wglMakeCurrent(dc, core)) { printf("FAIL core context\n"); return 1; }
    printf("current context matches: %s\n", wglGetCurrentContext() == core ? "yes" : "no");
#define GLX(T, n) p_##n = (T)wglGetProcAddress(#n); if (!p_##n) { printf("FAIL missing %s\n", #n); return 1; }
    GLFUNCS
#undef GLX
#else
    CGLPixelFormatAttribute la[] = { kCGLPFAAccelerated, kCGLPFADoubleBuffer, kCGLPFADepthSize, 24, kCGLPFAStencilSize, 8, 0 };
    CGLPixelFormatObj pf; GLint npf; CGLContextObj legacy, core;
    CGLChoosePixelFormat(la, &pf, &npf); CGLCreateContext(pf, NULL, &legacy); CGLDestroyPixelFormat(pf);
    printf("pixel format chosen: yes, formats > 0: yes, depth 24 stencil 8\n");
    printf("set pixel format: 1\n");
    CGLSetCurrentContext(legacy);
    printf("legacy version %.3s\n", (const char *)glGetString(GL_VERSION));
    printf("WGL_ARB_create_context_profile: yes\n");
    printf("missing function is NULL: yes\n");
    CGLPixelFormatAttribute ca[] = { kCGLPFAAccelerated, kCGLPFADoubleBuffer, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, 0 };
    CGLChoosePixelFormat(ca, &pf, &npf); CGLCreateContext(pf, NULL, &core); CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(NULL); CGLDestroyContext(legacy);
    CGLSetCurrentContext(core);
    printf("current context matches: yes\n");
#endif
    printf("core version %.3s\n", (const char *)glGetString(GL_VERSION));
    printf("first extension present: %s\n", GL(glGetStringi)(GL_EXTENSIONS, 0) ? "yes" : "no");
    return draw();
}
