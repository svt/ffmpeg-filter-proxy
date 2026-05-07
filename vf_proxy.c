/*
 * SPDX-FileCopyrightText: 2020 Sveriges Television AB
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "video.h"

typedef struct {
  const AVClass* class;
  char* filter_path;
  char* config;
  void* handle;
  void* user_data;
  int (*filter_init)(const char*, void**);
  int (*filter_frame)(unsigned char*,
                      unsigned int,
                      int,
                      int,
                      int,
                      double,
                      void*);
  void (*filter_uninit)(void*);

  // Per-instance BGRA scratch the proxied filter paints into.
  // Cairo-style premultiplied 8-bit; composited onto the 10-bit YUV input frame.
  uint8_t* scratch;
  int scratch_size;
  int scratch_linesize;
} ProxyContext;

#define OFFSET(x) offsetof(ProxyContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static av_cold int init(AVFilterContext* ctx) {
  ProxyContext* pc = ctx->priv;

  if (!pc->filter_path) {
    av_log(ctx, AV_LOG_ERROR, "no filter path provided!\n");
    return AVERROR(EINVAL);
  }

  pc->handle = dlopen(pc->filter_path, RTLD_LAZY);
  if (!pc->handle) {
    av_log(ctx, AV_LOG_ERROR, "%s\n", dlerror());
    return AVERROR(EINVAL);
  }

  char* error;
  pc->filter_init = dlsym(pc->handle, "filter_init");
  if ((error = dlerror()) != NULL) {
    av_log(ctx, AV_LOG_ERROR, "%s\n", error);
    dlclose(pc->handle);
    return AVERROR(EINVAL);
  }

  pc->filter_frame = dlsym(pc->handle, "filter_frame");
  if ((error = dlerror()) != NULL) {
    av_log(ctx, AV_LOG_ERROR, "%s\n", error);
    dlclose(pc->handle);
    return AVERROR(EINVAL);
  }

  pc->filter_uninit = dlsym(pc->handle, "filter_uninit");
  if ((error = dlerror()) != NULL) {
    av_log(ctx, AV_LOG_ERROR, "%s\n", error);
    dlclose(pc->handle);
    return AVERROR(EINVAL);
  }

  int rc;
  if ((rc = pc->filter_init(pc->config, &pc->user_data)) != 0) {
    av_log(ctx, AV_LOG_ERROR, "filter_init returned: %d\n", rc);
    dlclose(pc->handle);
    return AVERROR(EINVAL);
  }

  return 0;
}

static av_cold void uninit(AVFilterContext* ctx) {
  ProxyContext* pc = ctx->priv;

  av_freep(&pc->scratch);

  if (pc->handle) {
    if (pc->filter_uninit) {
      pc->filter_uninit(pc->user_data);
    }

    dlclose(pc->handle);
  }
}

typedef struct {
  float kr, kg, kb;
  int limited;
} ColorSpec;

static ColorSpec color_spec_for_frame(const AVFrame* f) {
  ColorSpec s = {0.2126f, 0.7152f, 0.0722f, 1};
  if (f->colorspace == AVCOL_SPC_BT470BG ||
      f->colorspace == AVCOL_SPC_SMPTE170M) {
    s.kr = 0.299f;
    s.kg = 0.587f;
    s.kb = 0.114f;
  }
  if (f->color_range == AVCOL_RANGE_JPEG) {
    s.limited = 0;
  }
  return s;
}

static inline void yuv_to_rgb(int y, int u, int v, ColorSpec s,
                              float* r_out, float* g_out, float* b_out) {
  float yn, un, vn;
  if (s.limited) {
    yn = (y - 64) * (1.0f / 876.0f);
    un = (u - 512) * (1.0f / 896.0f);
    vn = (v - 512) * (1.0f / 896.0f);
  } else {
    yn = y * (1.0f / 1023.0f);
    un = (u - 512) * (1.0f / 1023.0f);
    vn = (v - 512) * (1.0f / 1023.0f);
  }
  float cu = 2.0f * (1.0f - s.kb);
  float cv = 2.0f * (1.0f - s.kr);
  *r_out = yn + cv * vn;
  *g_out = yn - (s.kb * cu / s.kg) * un - (s.kr * cv / s.kg) * vn;
  *b_out = yn + cu * un;
}

static inline int rgb_to_y10(float r, float g, float b, ColorSpec s) {
  float yn = s.kr * r + s.kg * g + s.kb * b;
  int v = s.limited ? (int)lrintf(yn * 876.0f) + 64
                    : (int)lrintf(yn * 1023.0f);
  return av_clip(v, 0, 1023);
}

static inline void rgb_to_uv10(float r, float g, float b, ColorSpec s,
                               int* u_out, int* v_out) {
  float yn = s.kr * r + s.kg * g + s.kb * b;
  float un = (b - yn) / (2.0f * (1.0f - s.kb));
  float vn = (r - yn) / (2.0f * (1.0f - s.kr));
  if (s.limited) {
    *u_out = av_clip((int)lrintf(un * 896.0f) + 512, 0, 1023);
    *v_out = av_clip((int)lrintf(vn * 896.0f) + 512, 0, 1023);
  } else {
    *u_out = av_clip((int)lrintf(un * 1023.0f) + 512, 0, 1023);
    *v_out = av_clip((int)lrintf(vn * 1023.0f) + 512, 0, 1023);
  }
}

// Composite the BGRA scratch (premultiplied, 8-bit) onto a 10-bit YUV frame
// in place. Operates in non-linear RGB (matches cairo's sRGB-encoded output
// and what vf_overlay does), so subtitle/logo edges look identical to the
// previous split/overlay pipeline.
static void composite_bgra_on_yuv(AVFrame* dst, const ProxyContext* pc) {
  const ColorSpec spec = color_spec_for_frame(dst);
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(dst->format);
  const int sub_x = 1 << desc->log2_chroma_w;
  const int sub_y = 1 << desc->log2_chroma_h;
  const float inv_n = 1.0f / (float)(sub_x * sub_y);

  uint8_t* y_plane = dst->data[0];
  uint8_t* u_plane = dst->data[1];
  uint8_t* v_plane = dst->data[2];
  const int y_stride = dst->linesize[0];
  const int u_stride = dst->linesize[1];
  const int v_stride = dst->linesize[2];

  const int chroma_h = dst->height / sub_y;
  const int chroma_w = dst->width / sub_x;

  for (int cy = 0; cy < chroma_h; cy++) {
    for (int cx = 0; cx < chroma_w; cx++) {
      // Skip cells that the proxied filter didn't touch. Cairo writes
      // 0x00000000 for unpainted pixels (alpha 0 implies premul rgb 0), so a
      // zero alpha byte is sufficient.
      int touched = 0;
      for (int dy = 0; dy < sub_y && !touched; dy++) {
        const uint8_t* row = pc->scratch +
                             (cy * sub_y + dy) * pc->scratch_linesize +
                             cx * sub_x * 4;
        for (int dx = 0; dx < sub_x; dx++) {
          if (row[dx * 4 + 3] != 0) {
            touched = 1;
            break;
          }
        }
      }
      if (!touched) {
        continue;
      }

      uint8_t* up = u_plane + cy * u_stride + cx * 2;
      uint8_t* vp = v_plane + cy * v_stride + cx * 2;
      const int u = AV_RL16(up) & 0x3ff;
      const int v = AV_RL16(vp) & 0x3ff;

      float sum_r = 0, sum_g = 0, sum_b = 0;
      for (int dy = 0; dy < sub_y; dy++) {
        const int py = cy * sub_y + dy;
        const uint8_t* srow = pc->scratch + py * pc->scratch_linesize +
                              cx * sub_x * 4;
        uint8_t* yrow = y_plane + py * y_stride + cx * sub_x * 2;
        for (int dx = 0; dx < sub_x; dx++) {
          const uint8_t* sp = srow + dx * 4;
          uint8_t* yp = yrow + dx * 2;
          const int yi = AV_RL16(yp) & 0x3ff;
          float r, g, b;
          yuv_to_rgb(yi, u, v, spec, &r, &g, &b);
          const int a8 = sp[3];
          if (a8 != 0) {
            const float src_b = sp[0] * (1.0f / 255.0f);
            const float src_g = sp[1] * (1.0f / 255.0f);
            const float src_r = sp[2] * (1.0f / 255.0f);
            const float one_minus_a = (255 - a8) * (1.0f / 255.0f);
            r = src_r + r * one_minus_a;
            g = src_g + g * one_minus_a;
            b = src_b + b * one_minus_a;
            AV_WL16(yp, rgb_to_y10(r, g, b, spec));
          }
          sum_r += r;
          sum_g += g;
          sum_b += b;
        }
      }

      int new_u, new_v;
      rgb_to_uv10(sum_r * inv_n, sum_g * inv_n, sum_b * inv_n, spec,
                  &new_u, &new_v);
      AV_WL16(up, new_u);
      AV_WL16(vp, new_v);
    }
  }
}

static int filter_frame(AVFilterLink* inlink, AVFrame* in) {
  AVFilterContext* ctx = inlink->dst;
  AVFilterLink* outlink = ctx->outputs[0];
  ProxyContext* pc = ctx->priv;

  memset(pc->scratch, 0, pc->scratch_size);

  const double time_ms = in->pts * av_q2d(inlink->time_base) * 1000;
  const int rc = pc->filter_frame(pc->scratch, pc->scratch_size,
                                  in->width, in->height,
                                  pc->scratch_linesize, time_ms,
                                  pc->user_data);
  if (rc != 0) {
    av_log(ctx, AV_LOG_ERROR, "filter_frame returned: %d\n", rc);
    av_frame_free(&in);
    return AVERROR_UNKNOWN;
  }

  composite_bgra_on_yuv(in, pc);

  return ff_filter_frame(outlink, in);
}

static int config_input(AVFilterLink* inlink) {
  ProxyContext* pc = inlink->dst->priv;

  av_freep(&pc->scratch);
  pc->scratch_linesize = inlink->w * 4;
  pc->scratch_size = pc->scratch_linesize * inlink->h;
  pc->scratch = av_malloc(pc->scratch_size);
  if (!pc->scratch) {
    return AVERROR(ENOMEM);
  }
  return 0;
}

static const AVFilterPad inputs[] = {{
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
    .filter_frame = filter_frame,
    .config_props = config_input,
    .flags = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
}};

static const AVFilterPad outputs[] = {{
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
}};

static const AVOption proxy_options[] = {
    {"filter_path",
     "set the filter path",
     OFFSET(filter_path),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     CHAR_MIN,
     CHAR_MAX,
     FLAGS},
    {"config",
     "set the config",
     OFFSET(config),
     AV_OPT_TYPE_STRING,
     {.str = ""},
     CHAR_MIN,
     CHAR_MAX,
     FLAGS},
    {NULL},
};

AVFILTER_DEFINE_CLASS(proxy);

const FFFilter ff_vf_proxy = {
    .p.name = "proxy",
    .p.description = NULL_IF_CONFIG_SMALL("Video filter proxy."),
    .p.priv_class = &proxy_class,
    .priv_size = sizeof(ProxyContext),
    .init = init,
    .uninit = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_PIXFMTS(AV_PIX_FMT_YUV420P10LE,
                   AV_PIX_FMT_YUV422P10LE,
                   AV_PIX_FMT_YUV444P10LE),
};
