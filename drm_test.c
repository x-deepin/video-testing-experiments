/**
 * The test should run on virutal console without Xorg running
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

#include <X11/Xlib.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libdrm/i915_drm.h>
#include <libdrm/intel_bufmgr.h>
#include <libdrm/amdgpu_drm.h>
#include <libdrm/radeon_drm.h>
//FIXME: WTF nouveau_drm uses *class* as a struct field name, which failed to 
//compile using c++!
#include <libdrm/nouveau_drm.h>
#include <libdrm/vmwgfx_drm.h>

//include gbm before gl header
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <EGL/egl.h>

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

    int pflip_pending;
    int cleanup;
    int paused;
} dc = {-1, 0, };

static void err_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void err_quit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    exit(1);
    va_end(ap);
}

static int setup_drm()
{
    drmModeRes* resources;                  //resource array
    drmModeConnector* connector;            //connector array
    drmModeEncoder* encoder;                //encoder array

    if (!drmAvailable()) {
        err_quit("drm not loaded");
    }

    //open default dri device
    for (int i = 0; i < DRM_MAX_MINOR; i++) {
        char card[128] = {0};
        snprintf(card, 127, "/dev/dri/card%d", i);
        if (access(card, R_OK)) continue;

        int fd = open(card, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (fd < 0) {
            err_msg("open device failed: %s\n", strerror(errno));
            return 1;
        }

        //this need root, but it seems we do not need master here.
        /*if (drmSetMaster(fd)) {*/
            /*err_msg("setting master failed: %s\n", strerror(errno));*/
            /*close(fd);*/
            /*continue;*/
        /*}*/

        //try to acquire drm resources
        resources = drmModeGetResources(fd);
        if(resources == 0) {
            err_msg("drmModeGetResources failed\n");
            close(fd);
            continue;
        }

        //acquire drm connector
        int j;
        for (j = 0; j < resources->count_connectors; ++j) {
            connector = drmModeGetConnector(fd, resources->connectors[j]);
            if (!connector) continue;
            if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
                dc.conn = connector->connector_id;
                break; 
            }
            drmModeFreeConnector(connector);
        }
        // if there is no active connector, maybe best quit test silently
        if (j == resources->count_connectors) {
            err_msg("'card%d' No active connector found!\n", i);
            dc.fd = -2;
            close(fd);
            continue;
        }

        dc.fd = fd;
        err_msg("setup to test card%d\n", i);
        break;
    }

    if (dc.fd < 0) {
        err_msg("can not open any drm devices\n");
        // if no connectors at all, consider it not an error.
        return dc.fd == -1 ? 1:0;
    }

    int i;

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

    err_msg("Mode chosen [%s] : h: %u, v: %u\n",
            dc.mode.name, dc.mode.hdisplay, dc.mode.vdisplay);

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return 0;
}

/**
 * basically, if we can create an egl context, that means (I assume) drm hardware 
 * acceleration is working.
 */
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
    const char *ver, *extensions, *apis;

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
    apis = eglQueryString(dc.display, EGL_CLIENT_APIS);
    err_msg("ver: %s, ext: %s, apis: %s\n", ver, extensions, apis);

    if (!strstr(extensions, "EGL_KHR_surfaceless_context")) {
        err_quit("%s\n", "need EGL_KHR_surfaceless_context extension");
        exit(1);
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        err_quit("bind api failed\n");
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
    dc.pflip_pending = 0;
}

static void cleanup()
{
    if (dc.gbm) {
        drmEventContext ev;
        memset(&ev, 0, sizeof(ev));
        ev.version = DRM_EVENT_CONTEXT_VERSION;
        ev.page_flip_handler = modeset_page_flip_event;
        //ev.vblank_handler = modeset_vblank_handler;
        err_msg("wait for pending page-flip to complete...\n");
        while (dc.pflip_pending) {
            int ret = drmHandleEvent(dc.fd, &ev);
            if (ret)
                break;
        }

        eglDestroySurface(dc.display, dc.surface);
        eglDestroyContext(dc.display, dc.gl_context);
        eglTerminate(dc.display);

        if (dc.bo) {
            gbm_bo_destroy(dc.bo);
            gbm_bo_destroy(dc.next_bo);
        }

        if (dc.gbm_surface) {
            gbm_surface_destroy(dc.gbm_surface);
            gbm_device_destroy(dc.gbm);
        }
    }

    if (dc.fd >= 0) {
        if (dc.saved_crtc) {
            drmModeSetCrtc(dc.fd, dc.saved_crtc->crtc_id, dc.saved_crtc->buffer_id, 
                    dc.saved_crtc->x, dc.saved_crtc->y, &dc.conn, 1,
                    &dc.saved_crtc->mode);
            drmModeFreeCrtc(dc.saved_crtc);
        }
        drmDropMaster(dc.fd);
        close(dc.fd);
    }
}

static int doGEM(const char* chosen, int fd)
{
    int ret = 0;


    struct drm_gem_close clreq;
    void* ptr = NULL;
    memset(&clreq, 0, sizeof clreq);

    if (strcmp(chosen, "i915") == 0) {
        drm_intel_bufmgr* bufmgr;
        drm_intel_bo* bo;

        bufmgr = drm_intel_bufmgr_gem_init(fd, 4096);

        bo = drm_intel_bo_alloc(bufmgr, "test_bo", 4096, 0);
        if (!bo) {
            err_msg("drm_intel_bo_alloc failed\n");
            ret = 1;
            drm_intel_bufmgr_destroy(bufmgr);
            goto _out;
        }

        ret = drm_intel_bo_map(bo, 1);
        if (ret) {
            err_msg("drm_intel_bo_map failed\n");
            ret = 1;
            drm_intel_bo_unreference(bo);
            drm_intel_bufmgr_destroy(bufmgr);
            goto _out;
        }

        ptr = (void*)bo->virtual;
        err_msg("addr_ptr = %p, size %u\n", ptr, bo->size);

        for (int i = 0; i < bo->size/sizeof(int); i++) {
            *((int*)ptr+i) = i;
        }

        for (int i = 0; i < bo->size/sizeof(int); i++) {
            if (*((int*)ptr+i) != i) {
                err_msg("write and read failed\n");
                ret = 1;
                break;
            }
        }

        drm_intel_bo_unmap(bo);
        drm_intel_bo_unreference(bo);
        drm_intel_bufmgr_destroy(bufmgr);

    } else if (strcmp(chosen, "radeon") == 0) {
        struct drm_radeon_gem_create creq;
        memset(&creq, 0, sizeof creq);
        creq.size = (1<<20);
        creq.handle = 0;
        ret = drmCommandWriteRead(fd, DRM_RADEON_GEM_CREATE, &creq, sizeof creq);
        if (ret < 0) {
            err_msg("DRM_RADEON_GEM_CREATE failed\n");
            ret = 1;
            goto _out;
        }
        
        clreq.handle = creq.handle;

        struct drm_radeon_gem_mmap mreq;
        memset(&mreq, 0, sizeof mreq);
        mreq.handle = creq.handle;
        mreq.size = creq.size;
        ret = drmCommandWriteRead(fd, DRM_RADEON_GEM_MMAP, &mreq, sizeof mreq);
        if (ret < 0) {
            err_msg("DRM_RADEON_GEM_MMAP failed\n");
            drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &clreq);
            goto _out;
        }
        
        err_msg("addr_ptr = %p\n", mreq.addr_ptr);
        ptr = mmap(0, mreq.size, PROT_WRITE|PROT_READ, MAP_SHARED, fd, mreq.addr_ptr);
        if (ptr == MAP_FAILED) {
            err_msg("mmap failed: %s\n", strerror(errno));
            ret = 1;
            goto _out;
        }

        // if panic happens, this'll crash and considered as failure
        for (int i = 0; i < mreq.size/sizeof(int); i++) {
            *((int*)ptr+i) = i;
        }

        for (int i = 0; i < mreq.size/sizeof(int); i++) {
            if (*((int*)ptr+i) != i) {
                err_msg("write and read failed\n");
                ret = 1;
                break;
            }
        }
        munmap(ptr, mreq.size);
        drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &clreq);
    }

_out:
    return ret;
}

// test all available devices
static int TestGEM() 
{
    for (int i = 0; i < DRM_MAX_MINOR; i++) {
        char card[128] = {0};
        snprintf(card, 127, "/dev/dri/card%d", i);
        if (access(card, R_OK)) continue;

        err_msg("open '%s'\n", card);
        int fd = open(card, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (fd > 0) {
            drmVersionPtr ver = drmGetVersion(fd);
            if (!ver) {
                err_msg("drmGetVersion failed\n");
                close(fd);
                return 1;
            }

            err_msg("do gem test with %s...\n", ver->name);
            int ret = doGEM(ver->name, fd);
            drmFreeVersion(ver);
            close(fd);
            if (ret) return 1;
        } else {
            err_msg("%s\n", strerror(errno));
            if (errno != EINTR || errno != EAGAIN)
                return 1;
        }
    }

    return 0;
}

static int TestDevs() 
{
    //open default dri device
    for (int i = 0; i < DRM_MAX_MINOR; i++) {
        char card[128] = {0};
        snprintf(card, 127, "/dev/dri/card%d", i);
        if (access(card, R_OK)) continue;
        int fd = open(card, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (fd > 0) {

            //TODO: do basic test
            close(fd);
        }
    }
    return 0;
}

static int TestKMS() 
{
    memset(&dc, 0, sizeof dc);
    dc.fd = -1;
    if (setup_drm())
        return 1;
    cleanup();
    return 0;
}

static int TestRendering () 
{
    memset(&dc, 0, sizeof dc);
    dc.fd = -1;
    if (setup_drm())
        return 1;
    setup_egl();
    cleanup();

    return 0;
}

int main(int argc, char *argv[])
{
    
    typedef int (*TestFunc)();

    struct TestCase {
        const char* name; 
        TestFunc cb;
    } tests[] = {
        {"test open all drm devices", TestDevs},
        {"test kms", TestKMS},
        {"test gem", TestGEM},
        {"test rendering", TestRendering}
    };
    
    int success = 0;
    for (struct TestCase* tc = &tests[0]; tc != &tests[3]; tc++) {
        err_msg("\e[38;5;226mstart %s\e[00m\n", tc->name);
        if ((success = tc->cb())) {
            err_msg("\e[38;5;160m%s failed\e[00m\n", tc->name);
            break;
        } 
    }

    return success;
}
