#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <random>

#include <X11/Xlib.h>
//#include <GL/gl.h>

#include "glutil.h"
#include <EGL/egl.h>

#define err_msg(...) do { \
    fprintf(stderr, __VA_ARGS__); \
} while (0)

#define err_quit(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    exit(1); \
} while (0)


#include <iostream>
using namespace std;

struct context_ {
    int fd;
    int width, height;
    Display* xdisplay;
    EGLDisplay display;
    EGLContext context;

    EGLSurface surface;
    Window window;

    GLProcess* proc;
} dc = {
    0, 400, 300,
};
int fd;

static const char* vert_shader = R"(
attribute vec2 position;

void main() {
    gl_Position = vec4(position.xy, 0.0, 1.0);
}
)";
static const char* frag_shader = R"(
uniform mediump float red;
void main() {
    gl_FragColor = vec4(red, 0.0, 0.0, 1.0);
}
)";

static void setup_egl()
{
    dc.xdisplay = XOpenDisplay(NULL);

    EGLint major;
    EGLint minor;
    const char *ver, *extensions, *apis;

    dc.display = eglGetDisplay((EGLNativeDisplayType)dc.xdisplay);
    eglInitialize(dc.display, &major, &minor);
    ver = eglQueryString(dc.display, EGL_VERSION);
    extensions = eglQueryString(dc.display, EGL_EXTENSIONS);
    apis = eglQueryString(dc.display, EGL_CLIENT_APIS);
    //err_msg("ver: %s, ext: %s, apis: %s\n", ver, extensions, apis);

    static const EGLint conf_att[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig conf;
    int num_conf;
    EGLBoolean ret = eglChooseConfig(dc.display, conf_att, &conf, 1, &num_conf);
    if (!ret || num_conf != 1) {
        err_quit("cannot find a proper EGL framebuffer configuration");
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        err_quit("EGL_OPENGL_API is not supported.\n");
    }


    static const EGLint ctx_att[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    dc.context = eglCreateContext(dc.display, conf, EGL_NO_CONTEXT, ctx_att);
    if (dc.context == EGL_NO_CONTEXT) {
        err_quit("no context created.\n");
    }

    EGLint visual_id;
    eglGetConfigAttrib(dc.display, conf, EGL_NATIVE_VISUAL_ID, &visual_id);

    Window root = DefaultRootWindow(dc.xdisplay);
    unsigned long mask = 0;
    XSetWindowAttributes attrs;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
    mask = CWBackPixel | CWBorderPixel | CWEventMask;


    XVisualInfo visualInfo_tmpl;
    int nitem;
    visualInfo_tmpl.visualid = visual_id;
    XVisualInfo* visualInfo = XGetVisualInfo(dc.xdisplay, VisualIDMask, &visualInfo_tmpl, &nitem);
    if (!visualInfo) {
        err_quit("XGetVisualInfo failed\n");
    }

    dc.window = XCreateWindow(dc.xdisplay, root, 0, 0, dc.width, dc.height, 
            0, visualInfo->depth, InputOutput, visualInfo->visual, mask, &attrs);
    XMapWindow(dc.xdisplay, dc.window);
    
    XFree(visualInfo);

    dc.surface = eglCreateWindowSurface(dc.display, conf,
            (EGLNativeWindowType)dc.window, NULL);
    if (dc.surface == EGL_NO_SURFACE) {
        err_quit("cannot create EGL window surface");
    }

    if (!eglMakeCurrent(dc.display, dc.surface, dc.surface, dc.context)) {
        err_quit("cannot activate EGL context");
    }

}

static void render()
{
    static float red = 0.0;
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1f(glGetUniformLocation(dc.proc->program, "red"), red);
    red += 0.01;
    if (red > 1.0) red = 0.0;

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static long get_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(int argc, char *argv[])
{
    setup_egl();

    glViewport(0, 0, dc.width, dc.height);
    
    //glClearColor(1.0, 1.0, 1.0, 1.0);
    dc.proc = glprocess_create(vert_shader, frag_shader, true);
    if (!dc.proc) return false;

    glGenBuffers(1, &dc.proc->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, dc.proc->vbo);

    GLfloat vertex_data[] = {
        -0.5, -0.5,
        -0.5, 0.5,
         0.5, 0.5,

         0.5, 0.5,
         0.5, -0.5,
        -0.5, -0.5,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof vertex_data, vertex_data, GL_STATIC_DRAW);

    glUseProgram(dc.proc->program);

    GLint pos_attrib = glGetAttribLocation(dc.proc->program, "position");
    glEnableVertexAttribArray(pos_attrib);
    glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    long ts = get_time();
    long start = ts;
    int stop = 0;
    while (!stop && get_time() - start < 3000) {
        XEvent ev;
        while (XPending(dc.xdisplay)) {
            XNextEvent(dc.xdisplay, &ev);
            switch(ev.type) {
                case Expose:
                    err_msg("expose\n");
                    break;
            }
        }

        long duration = get_time() - ts;
        if (duration >= 30) {
            render();
            eglSwapBuffers(dc.display, dc.surface);
            ts = get_time();
        }
    }

    glprocess_release(dc.proc);

    eglDestroySurface(dc.display, dc.surface);
    eglDestroyContext(dc.display, dc.context);
    eglTerminate(dc.display);

    XDestroyWindow(dc.xdisplay, dc.window);
    XCloseDisplay(dc.xdisplay);
    return 0;
}
