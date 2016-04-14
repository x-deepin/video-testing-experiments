#include <X11/Xlib.h>
#define GLEW_STATIC
#include <GL/glew.h>
#include <GL/glx.h>

#include <xcb/xcb.h>

#define err_quit(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    exit(1); \
} while (0)


#include <iostream>
using namespace std;

static Window dummy;
static GLXContext ctx;

int main(int argc, char *argv[])
{
    //TODO: create glx context first
    //
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        err_quit("glewInit failed\n");
    }
    
    if (!GLEW_ARB_shader_texture_lod) {
        err_quit("GL_ARB_shader_texture_lod not defined\n");
    }
    return 0;
}
