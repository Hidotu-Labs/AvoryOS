/**************************************************************************
 *
 * Copyright (C) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#include <check.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <virglrenderer.h>
#include "virgl_hw.h"
#include "vrend/vrend_iov.h"
#include "util/u_formats.h"
#include "testvirgl_encode.h"
#include "virgl_protocol.h"
#include "util/u_memory.h"

#include <stdio.h>
#include <epoxy/gl.h>
static const uint32_t test_green=0xff00ff00;
struct vertex {
   float position[4];
   float color[4];
};

static struct vertex vertices[3] =
{
   {
      { 0.0f, -0.9f, 0.0f, 1.0f },
      { 1.0f, 0.0f, 0.0f, 1.0f }
   },
   {
      { -0.9f, 0.9f, 0.0f, 1.0f },
      { 0.0f, 1.0f, 0.0f, 1.0f }
   },
   {
      { 0.9f, 0.9f, 0.0f, 1.0f },
      { 0.0f, 0.0f, 1.0f, 1.0f }
   }
};

int main(void)
{
    struct virgl_context ctx;
    struct virgl_resource res;
    struct virgl_resource vbo, ibo;
    struct virgl_surface surf;
    struct pipe_framebuffer_state fb_state;
    struct pipe_vertex_element ve[2];
    struct pipe_vertex_buffer vbuf;
    int ve_handle, vs_handle, fs_handle;
    int ctx_handle = 1;
    union pipe_color_union color;
    struct virgl_box box;
    int ret;
    int tw = 300, th = 300;

    context_flags |= VIRGL_RENDERER_USE_SURFACELESS;
    ret = testvirgl_init_ctx_cmdbuf(&ctx, context_flags);
    ck_assert_int_eq(ret, 0);

    fprintf(stderr,"HOST_RENDERER: %s\n",glGetString(GL_RENDERER));

    /* init and create simple 2D resource */
    ret = testvirgl_create_backed_simple_2d_res(&res, 1, tw, th);
    ck_assert_int_eq(ret, 0);

    /* attach resource to context */
    virgl_renderer_ctx_attach_resource(ctx.ctx_id, res.handle);

    /* create a surface for the resource */
    memset(&surf, 0, sizeof(surf));
    surf.base.format = PIPE_FORMAT_B8G8R8X8_UNORM;
    surf.handle = ctx_handle++;
    surf.base.texture = &res.base;

    virgl_encoder_create_surface(&ctx, surf.handle, &res, &surf.base);

    /* set the framebuffer state */
    fb_state.nr_cbufs = 1;
    fb_state.zsbuf = NULL;
    fb_state.cbufs[0] = &surf.base;
    virgl_encoder_set_framebuffer_state(&ctx, &fb_state);

    /* clear the resource */
    /* clear buffer to green */
    color.f[0] = 0.0;
    color.f[1] = 1.0;
    color.f[2] = 0.0;
    color.f[3] = 1.0;
    virgl_encode_clear(&ctx, PIPE_CLEAR_COLOR0, &color, 0.0, 0);

    /* create vertex elements */
    ve_handle = ctx_handle++;
    memset(ve, 0, sizeof(ve));
    ve[0].src_offset = Offset(struct vertex, position);
    ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    ve[1].src_offset = Offset(struct vertex, color);
    ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    virgl_encoder_create_vertex_elements(&ctx, ve_handle, 2, ve);

    virgl_encode_bind_object(&ctx, ve_handle, VIRGL_OBJECT_VERTEX_ELEMENTS);

    /* create vbo */
    ret = testvirgl_create_backed_simple_buffer(&vbo, 2, sizeof(vertices), PIPE_BIND_VERTEX_BUFFER);
    ck_assert_int_eq(ret, 0);
    virgl_renderer_ctx_attach_resource(ctx.ctx_id, vbo.handle);

    /* inline write the data to it */
    box.x = 0;
    box.y = 0;
    box.z = 0;
    box.w = sizeof(vertices);
    box.h = 1;
    box.d = 1;
    virgl_encoder_inline_write(&ctx, &vbo, 0, 0, (struct pipe_box *)&box, &vertices, box.w, 0);

    vbuf.stride = sizeof(struct vertex);
    vbuf.buffer_offset = 0;
    vbuf.buffer = &vbo.base;
    virgl_encoder_set_vertex_buffers(&ctx, 1, &vbuf);

    /* create vertex shader */
    {
        struct pipe_shader_state vs;
         const char *text =
           "VERT\n"
           "DCL IN[0]\n"
           "DCL IN[1]\n"
           "DCL OUT[0], POSITION\n"
           "DCL OUT[1], COLOR\n"
           "  0: MOV OUT[1], IN[1]\n"
           "  1: MOV OUT[0], IN[0]\n"
           "  2: END\n";
         memset(&vs, 0, sizeof(vs));
         vs_handle = ctx_handle++;
         virgl_encode_shader_state(&ctx, vs_handle, PIPE_SHADER_VERTEX,
                                   &vs, text);
         virgl_encode_bind_shader(&ctx, vs_handle, PIPE_SHADER_VERTEX);
    }

    /* create fragment shader */
    {
        struct pipe_shader_state fs;
        const char *text =
            "FRAG\n"
            "DCL IN[0], COLOR, LINEAR\n"
            "DCL OUT[0], COLOR\n"
            "  0: MOV OUT[0], IN[0]\n"
            "  1: END\n";
        memset(&fs, 0, sizeof(fs));
        fs_handle = ctx_handle++;
        virgl_encode_shader_state(&ctx, fs_handle, PIPE_SHADER_FRAGMENT,
                                   &fs, text);

        virgl_encode_bind_shader(&ctx, fs_handle, PIPE_SHADER_FRAGMENT);
    }

    /* link shader */
    {
        uint32_t handles[PIPE_SHADER_TYPES];
        memset(handles, 0, sizeof(handles));
        handles[PIPE_SHADER_VERTEX] = vs_handle;
        handles[PIPE_SHADER_FRAGMENT] = fs_handle;
        virgl_encode_link_shader(&ctx, handles);
    }

    /* set blend state */
    {
        struct pipe_blend_state blend;
        int blend_handle = ctx_handle++;
        memset(&blend, 0, sizeof(blend));
        blend.rt[0].colormask = PIPE_MASK_RGBA;
        virgl_encode_blend_state(&ctx, blend_handle, &blend);
        virgl_encode_bind_object(&ctx, blend_handle, VIRGL_OBJECT_BLEND);
    }

    /* set depth stencil alpha state */
    {
        struct pipe_depth_stencil_alpha_state dsa;
        int dsa_handle = ctx_handle++;
        memset(&dsa, 0, sizeof(dsa));
        dsa.depth.writemask = 1;
        dsa.depth.func = PIPE_FUNC_LESS;
        virgl_encode_dsa_state(&ctx, dsa_handle, &dsa);
        virgl_encode_bind_object(&ctx, dsa_handle, VIRGL_OBJECT_DSA);
    }

    /* set rasterizer state */
    {
        struct pipe_rasterizer_state rasterizer;
        int rs_handle = ctx_handle++;
        memset(&rasterizer, 0, sizeof(rasterizer));
        rasterizer.cull_face = PIPE_FACE_NONE;
        rasterizer.half_pixel_center = 1;
        rasterizer.bottom_edge_rule = 1;
        rasterizer.depth_clip = 1;
        virgl_encode_rasterizer_state(&ctx, rs_handle, &rasterizer);
        virgl_encode_bind_object(&ctx, rs_handle, VIRGL_OBJECT_RASTERIZER);
    }

    /* set viewport state */
    {
        struct pipe_viewport_state vp;
        float znear = 0, zfar = 1.0;
        float half_w = tw / 2.0f;
        float half_h = th / 2.0f;
        float half_d = (zfar - znear) / 2.0f;

        vp.scale[0] = half_w;
        vp.scale[1] = half_h;
        vp.scale[2] = half_d;

        vp.translate[0] = half_w + 0;
        vp.translate[1] = half_h + 0;
        vp.translate[2] = half_d + znear;
        virgl_encoder_set_viewport_states(&ctx, 0, 1, &vp);
    }

    /* draw */
    {
        struct pipe_draw_info info;
        memset(&info, 0, sizeof(info));
        info.count = 3;
        info.mode = PIPE_PRIM_TRIANGLES;
        virgl_encoder_draw_vbo(&ctx, &info);
    }

    ret = testvirgl_ctx_send_cmdbuf(&ctx);
    ck_assert_int_eq(ret, 0);

    /* Reproduce the captured SET_VERTEX_BUFFERS(0) -> indexed instanced draw. */
    ret = testvirgl_create_backed_simple_buffer(&ibo, 3, 6, PIPE_BIND_INDEX_BUFFER);
    ck_assert_int_eq(ret, 0);
    virgl_renderer_ctx_attach_resource(ctx.ctx_id, ibo.handle);
    uint16_t indices[] = {0,1,2};
    struct pipe_box ibox = {.width=6,.height=1,.depth=1};
    virgl_encoder_inline_write(&ctx,&ibo,0,0,&ibox,indices,6,0);
    struct pipe_index_buffer ib={.index_size=2,.buffer=&ibo.base};
    virgl_encoder_set_index_buffer(&ctx,&ib);
    virgl_encoder_set_vertex_buffers(&ctx,0,NULL);
    struct pipe_draw_info indexed={.indexed=true,.mode=PIPE_PRIM_TRIANGLES,.count=3,.instance_count=1,.max_index=~0U};
    fprintf(stderr,"REPRO: submitting indexed draw with missing vertex buffers\n");
    virgl_encoder_draw_vbo(&ctx,&indexed);
    ck_assert_int_eq(testvirgl_ctx_send_cmdbuf(&ctx),0);
    /* The invalid draw must be skipped and subsequent valid geometry survive. */
    virgl_encoder_set_vertex_buffers(&ctx,1,&vbuf);
    virgl_encoder_draw_vbo(&ctx,&indexed);
    ck_assert_int_eq(testvirgl_ctx_send_cmdbuf(&ctx),0);
    fprintf(stderr,"REPRO: valid indexed draw after missing buffers survived\n");
    /* create a fence */
    testvirgl_reset_fence();
    virgl_renderer_create_fence(1, ctx.ctx_id);

    do {
        int fence;

        virgl_renderer_poll();
        fence = testvirgl_get_last_fence();
        if (fence >= 1)
            break;
        nanosleep((struct timespec[]){{0, 50000}}, NULL);
    } while(1);

    /* read back the tri values in the resource */
    box.x = 0;
    box.y = 0;
    box.z = 0;
    box.w = tw;
    box.h = th;
    box.d = 1;
    ret = virgl_renderer_transfer_read_iov(res.handle, ctx.ctx_id, 0, 0, 0, &box, 0, NULL, 0);
    ck_assert_int_eq(ret, 0);

    {
        int w, h;
        bool all_cleared = true;
        uint32_t *ptr = res.iovs[0].iov_base;
        for (h = 0; h < th; h++) {
            for (w = 0; w < tw; w++) {
                if (ptr[h * tw + w] != test_green)
                    all_cleared = false;
            }
        }
        ck_assert_int_eq(all_cleared, false);
    }

    /* cleanup */
    virgl_renderer_ctx_detach_resource(ctx.ctx_id, res.handle);

    testvirgl_destroy_backed_res(&ibo);
    testvirgl_destroy_backed_res(&vbo);
    testvirgl_destroy_backed_res(&res);

    testvirgl_fini_ctx_cmdbuf(&ctx);
    puts("PASS: invalid indexed draw skipped; subsequent indexed geometry and pixel readback survived");
    return 0;
}
