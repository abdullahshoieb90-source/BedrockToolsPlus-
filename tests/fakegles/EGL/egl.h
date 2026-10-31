#pragma once
using EGLDisplay = void*;
using EGLContext = void*;
#define EGL_NO_DISPLAY nullptr
#define EGL_NO_CONTEXT nullptr
EGLDisplay eglGetCurrentDisplay();
EGLContext eglGetCurrentContext();
