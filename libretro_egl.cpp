#include <cstdio>

#include <GL/gl.h>
//#include <GL/glcorearb.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "libretro_egl.h"

static bool s_init_done = false;
static EGLDisplay s_display;
static EGLContext s_context;

void setup_egl_context() {
	if (s_init_done)
		return;

	const EGLint configAttribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_RED_SIZE, 8,
		EGL_DEPTH_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE
	};

	const EGLint contextAttribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK,  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_CONTEXT_FLAGS_KHR, EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR,
	};

#if 0
	const int pbufferWidth = 1024;
	const int pbufferHeight = 512;

	const EGLint pbufferAttribs[] = {
		EGL_WIDTH, pbufferWidth,
		EGL_HEIGHT, pbufferHeight,
		EGL_NONE,
	};
#endif

	// 1. Initialize EGL
	s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (s_display == EGL_NO_DISPLAY) {
		fprintf(stderr, "EGL: Failed to get a display (0x%x)\n", eglGetError());
		return;
	}

	if (!eglInitialize(s_display, NULL, NULL)) {
		fprintf(stderr, "EGL: Failed to initialize the display (0x%x)\n", eglGetError());
		return;
	}

	// 2. Select an appropriate configuration
	EGLint numConfigs;
	EGLConfig eglCfg;

	eglChooseConfig(s_display, configAttribs, &eglCfg, 1, &numConfigs);
	if (numConfigs == 0) {
		fprintf(stderr, "EGL: Failed to get a frame buffer config (0x%x)\n", eglGetError());
		return;
	}

#if 0
	// 3. Create a surface
	EGLSurface eglSurf = eglCreatePbufferSurface(s_display, eglCfg,
			pbufferAttribs);
#endif

	// 4. Bind the API
	eglBindAPI(EGL_OPENGL_API);

	// 5. Create a context and make it current
	s_context = eglCreateContext(s_display, eglCfg, EGL_NO_CONTEXT, NULL);
	if (s_context == EGL_NO_CONTEXT) {
		fprintf(stderr, "EGL: Failed to create the drawing context (0x%x)\n", eglGetError());
		return;
	}

	eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_context);

	// Sanity
	const GLubyte* s = glGetString(GL_VERSION);
	fprintf(stderr, "OpenGL ctxt => %s\n", s);

	// from now on use your OpenGL context
	s_init_done = true;
}

void destroy_egl_context() {
	if (!s_init_done)
		return;

	eglMakeCurrent(s_display, NULL, NULL, NULL);
	eglDestroyContext(s_display, s_context);

	eglTerminate(s_display);

	s_init_done = false;
}
