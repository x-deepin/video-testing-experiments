#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

//include gbm before gl header
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <EGL/egl.h>

#include <iostream>
#include <string>
#include <algorithm>

using namespace std;

struct DisplayContext {
    int fd;                                 //drm device handle
    EGLDisplay display;
    EGLContext gl_context;

    drmModeModeInfo mode;
    uint32_t conn; // connector id
    uint32_t crtc; // crtc id
    drmModeCrtc *saved_crtc;

    struct gbm_device *gbm;
    struct gbm_surface *gbm_surface;
    EGLSurface surface;

    struct gbm_bo *bo;
    struct gbm_bo *next_bo;
    uint32_t next_fb_id; 

    bool pflip_pending;
    bool cleanup;
    bool paused;
} dc = {0};

static void err_quit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    exit(1);
    va_end(ap);
}

static int open_drm_device()
{
    //open default dri device
    for (int i = 0; i < 16; i++) {
        char card[128] = {0};
        snprintf(card, 127, "/dev/dri/card%d", i);
        std::cerr << "open " << card << endl;
        int fd = open(card, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (fd >= 0) return fd;
    }

    return -1;
}

static void setup_drm()
{
    drmModeRes* resources;                  //resource array
    drmModeConnector* connector;            //connector array
    drmModeEncoder* encoder;                //encoder array

    dc.fd = open_drm_device();
    if (dc.fd <= 0) { 
        err_quit(strerror(errno));
    }

    drmSetMaster(dc.fd);
    //acquire drm resources
    resources = drmModeGetResources(dc.fd);
    if(resources == 0) {
        err_quit("drmModeGetResources failed");
    }

    int i;
    //acquire drm connector
    for (i = 0; i < resources->count_connectors; ++i) {
        connector = drmModeGetConnector(dc.fd,resources->connectors[i]);
        if (!connector) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            dc.conn = connector->connector_id;
            std::cerr << "find connected connector id " << dc.conn << std::endl;
            break; 
        }
        drmModeFreeConnector(connector);
    }

    if (i == resources->count_connectors) {
        err_quit("No active connector found!");
    }

    encoder = NULL;
    if (connector->encoder_id) {
        encoder = drmModeGetEncoder(dc.fd, connector->encoder_id);
        if(encoder) {
            dc.crtc = encoder->crtc_id;
            drmModeCrtc* crtc = drmModeGetCrtc(dc.fd, dc.crtc);
            dc.mode = crtc->mode;
            drmModeFreeEncoder(encoder);
        }
    }

    if (!encoder) {
        std::cerr << "connector has no encoder";
        for(i = 0; i < resources->count_encoders; ++i) {
            encoder = drmModeGetEncoder(dc.fd,resources->encoders[i]);
            if(encoder==0) { continue; }
            for (int j = 0; j < resources->count_crtcs; ++j) {
                if (encoder->possible_crtcs & (1<<j)) {
                    dc.crtc = resources->crtcs[j];
                    break;
                }
            }
            drmModeFreeEncoder(encoder);
            if (dc.crtc) break;
        }

        if (i == resources->count_encoders) {
            err_quit("No active encoder found!");
        }
        dc.mode = connector->modes[0];
    }

    dc.saved_crtc = drmModeGetCrtc(dc.fd, dc.crtc);

    fprintf(stderr, "\tMode chosen [%s] : Clock => %d, Vertical refresh => %d, Type => %d\n",
            dc.mode.name, dc.mode.clock, dc.mode.vrefresh, dc.mode.type);

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
}

static void setup_egl()
{
    dc.gbm = gbm_create_device(dc.fd);
    printf("backend name: %s\n", gbm_device_get_backend_name(dc.gbm));

    dc.gbm_surface = gbm_surface_create(dc.gbm, dc.mode.hdisplay,
            dc.mode.vdisplay, GBM_FORMAT_XRGB8888,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!dc.gbm_surface) {
        printf("cannot create gbm surface (%d): %m", errno);
        exit(-EFAULT);
    }


    EGLint major;
    EGLint minor;
    const char *ver, *extensions;

    static const EGLint conf_att[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 1,
        EGL_NONE,
    };
    static const EGLint ctx_att[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    dc.display = eglGetDisplay(dc.gbm);
    eglInitialize(dc.display, &major, &minor);
    ver = eglQueryString(dc.display, EGL_VERSION);
    extensions = eglQueryString(dc.display, EGL_EXTENSIONS);
    fprintf(stderr, "ver: %s, ext: %s\n", ver, extensions);

    if (!strstr(extensions, "EGL_KHR_surfaceless_context")) {
        fprintf(stderr, "%s\n", "need EGL_KHR_surfaceless_context extension");
        exit(1);
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        std::cerr << "bind api failed" << std::endl;
        exit(-1);
    }

    EGLConfig conf;
    int num_conf;
    EGLBoolean ret = eglChooseConfig(dc.display, conf_att, &conf, 1, &num_conf);
    if (!ret || num_conf != 1) {
        printf("cannot find a proper EGL framebuffer configuration");
        exit(-1);
    }

    dc.gl_context = eglCreateContext(dc.display, conf, EGL_NO_CONTEXT, ctx_att);
    if (dc.gl_context == EGL_NO_CONTEXT) {
        printf("no context created.\n"); exit(0);
    }

    dc.surface = eglCreateWindowSurface(dc.display, conf,
            (EGLNativeWindowType)dc.gbm_surface,
            NULL);
    if (dc.surface == EGL_NO_SURFACE) {
        printf("cannot create EGL window surface");
        exit(-1);
    }

    if (!eglMakeCurrent(dc.display, dc.surface, dc.surface, dc.gl_context)) {
        printf("cannot activate EGL context");
        exit(-1);
    }
}

static void modeset_page_flip_event(int fd, unsigned int frame,
        unsigned int sec, unsigned int usec,
        void *data)
{
    //if (!dc.paused)
        dc.pflip_pending = false;
}

static void cleanup()
{
    cerr << __func__ << std::endl;

    drmEventContext ev;
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = modeset_page_flip_event;
    //ev.vblank_handler = modeset_vblank_handler;
    std::cerr << "wait for pending page-flip to complete..." << std::endl;
    while (dc.pflip_pending) {
        int ret = drmHandleEvent(dc.fd, &ev);
        if (ret)
            break;
    }

    eglDestroySurface(dc.display, dc.surface);
    eglDestroyContext(dc.display, dc.gl_context);
    eglTerminate(dc.display);

    if (dc.bo) {
        std::cerr << "destroy bo: " << (unsigned long)dc.bo << std::endl;
        gbm_bo_destroy(dc.bo);
        std::cerr << "destroy bo: " << (unsigned long)dc.next_bo << std::endl;
        gbm_bo_destroy(dc.next_bo);
    }

    if (dc.gbm_surface) {
        gbm_surface_destroy(dc.gbm_surface);
        gbm_device_destroy(dc.gbm);
    }

    if (dc.pflip_pending) std::cerr << "still has pflip pending" << std::endl;
    if (dc.saved_crtc) {
        drmModeSetCrtc(dc.fd, dc.saved_crtc->crtc_id, dc.saved_crtc->buffer_id, 
                dc.saved_crtc->x, dc.saved_crtc->y, &dc.conn, 1, &dc.saved_crtc->mode);
        drmModeFreeCrtc(dc.saved_crtc);
    }
    drmDropMaster(dc.fd);
    close(dc.fd);
}

int main(int argc, char *argv[])
{
    setup_drm();    

    setup_egl();

    cleanup();
    return 0;
}
