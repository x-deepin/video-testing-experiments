#include <glib.h>
#include <cstdlib>
#include <cstdint>
#define COGL_ENABLE_EXPERIMENTAL_2_0_API
#include <cogl/cogl.h>

#define FB_WIDTH 512
#define FB_HEIGHT 512

CoglContext *test_ctx;
CoglFramebuffer *test_fb;

static CoglBool check_flags(CoglRenderer *renderer) {
    if (cogl_renderer_get_driver (renderer) != COGL_DRIVER_GL &&
            cogl_renderer_get_driver (renderer) != COGL_DRIVER_GL3) {
        return FALSE;
    }

#if 0
    //wait for cogl patch merged
    if (!cogl_has_feature (test_ctx, COGL_FEATURE_ID_SHADER_TEXTURE_LOD)) {
        return FALSE;
    }
#endif

    if (!cogl_has_feature (test_ctx, COGL_FEATURE_ID_TEXTURE_NPOT)) {
        return FALSE;
    }

    if (!cogl_has_feature (test_ctx, COGL_FEATURE_ID_GLSL)) {
        return FALSE;
    }

    if (!cogl_has_feature (test_ctx, COGL_FEATURE_ID_OFFSCREEN)) {
        return FALSE;
    }

    return TRUE;
}

static void init()
{
    CoglError *error = NULL;
    CoglDisplay *display;
    CoglRenderer *renderer;
    CoglBool missing_requirement;

    //g_setenv ("COGL_X11_SYNC", "1", 0);

    test_ctx = cogl_context_new (NULL, &error);
    if (!test_ctx) {
        g_message ("Failed to create a CoglContext: %s", error->message);
        exit(1);
    }

    display = cogl_context_get_display (test_ctx);
    renderer = cogl_display_get_renderer (display);

    if (!check_flags (renderer)) {
        g_message ("WARNING: Missing required feature[s] for this test\n");
        exit(1);
    }

    {
        //TODO: get all monitors size to test
        CoglOffscreen *offscreen;
        CoglTexture2D *tex = cogl_texture_2d_new_with_size (test_ctx,
                FB_WIDTH, FB_HEIGHT);
        offscreen = cogl_offscreen_new_with_texture (COGL_TEXTURE (tex));
        test_fb = COGL_FRAMEBUFFER (offscreen);
    }

    if (!cogl_framebuffer_allocate (test_fb, &error)) {
        g_message ("Failed to allocate framebuffer: %s", error->message);
        exit(1);
    }

    cogl_framebuffer_clear4f (test_fb,
            COGL_BUFFER_BIT_COLOR |
            COGL_BUFFER_BIT_DEPTH |
            COGL_BUFFER_BIT_STENCIL,
            0, 0, 0, 1);
}

static void cleanup(void)
{
  if (test_fb) cogl_object_unref(test_fb);

  if (test_ctx) cogl_object_unref(test_ctx);
}

static CoglTexture * test_texture_new_with_size(CoglContext *ctx,
                                  int width, int height,
                                  CoglTextureComponents components)
{
  CoglTexture *tex;
  CoglError *skip_error = NULL;

  tex = COGL_TEXTURE (cogl_texture_2d_new_with_size (ctx, width, height));
  cogl_texture_set_components(tex, components);
  cogl_primitive_texture_set_auto_mipmap(tex, TRUE);

  if (!cogl_texture_allocate(tex, &skip_error)) {
      cogl_error_free(skip_error);
      cogl_object_unref(tex);
      return NULL;
  }

  return tex;
}

int main(int argc, char *argv[])
{
    init();

    CoglTexture* tex = test_texture_new_with_size(test_ctx, 
            1440, 900, COGL_TEXTURE_COMPONENTS_RGBA);
    cogl_object_unref(tex);

    cleanup();
    return 0;
}
