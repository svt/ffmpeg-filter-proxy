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
#include "filters.h"
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
  // Optional. If exported, the proxy reuses the previous scratch render
  // when the version returned for the current `ts_millis` matches the
  // previously rendered version.
  uint64_t (*filter_version)(double, void*);

  // Per-instance BGRA scratch the proxied filter paints into.
  // Cairo-style premultiplied 8-bit; composited onto the 10-bit YUV input frame.
  uint8_t* scratch;
  int scratch_size;
  int scratch_linesize;

  uint64_t cached_version;
  int cache_present;
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

  pc->filter_version = dlsym(pc->handle, "filter_version");
  (void)dlerror();

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

// All values needed to convert 10-bit YUV ↔ normalized RGB for a given frame.
// Hoisted here so the inner per-pixel loop multiplies, doesn't divide.
typedef struct {
  float kr, kg, kb;
  // YUV→RGB coefficients (chroma → R, G, B contributions).
  float cu;   //  2 * (1 - kb)
  float cv;   //  2 * (1 - kr)
  float cgu;  // -kb * cu / kg  (U contribution to G)
  float cgv;  // -kr * cv / kg  (V contribution to G)
  // Range scaling.
  float y_in_scale;   // (yn from y) = (y - y_offset_in) * y_in_scale
  float uv_in_scale;  // (un from u) = (u - 512) * uv_in_scale
  float y_out_scale;  // (y from yn) = round(yn * y_out_scale) + y_offset_out
  float uv_out_scale;
  int y_offset_in;
  int y_offset_out;
} ColorSpec;

static ColorSpec color_spec_for_frame(const AVFrame* f) {
  float kr = 0.2126f, kg = 0.7152f, kb = 0.0722f;
  if (f->colorspace == AVCOL_SPC_BT470BG ||
      f->colorspace == AVCOL_SPC_SMPTE170M) {
    kr = 0.299f;
    kg = 0.587f;
    kb = 0.114f;
  }
  const int limited = (f->color_range != AVCOL_RANGE_JPEG);
  const float cu = 2.0f * (1.0f - kb);
  const float cv = 2.0f * (1.0f - kr);
  ColorSpec s = {
      .kr = kr,
      .kg = kg,
      .kb = kb,
      .cu = cu,
      .cv = cv,
      .cgu = -kb * cu / kg,
      .cgv = -kr * cv / kg,
      .y_in_scale = limited ? 1.0f / 876.0f : 1.0f / 1023.0f,
      .uv_in_scale = limited ? 1.0f / 896.0f : 1.0f / 1023.0f,
      .y_out_scale = limited ? 876.0f : 1023.0f,
      .uv_out_scale = limited ? 896.0f : 1023.0f,
      .y_offset_in = limited ? 64 : 0,
      .y_offset_out = limited ? 64 : 0,
  };
  return s;
}

static inline void yuv_to_rgb(int y, int u, int v, const ColorSpec* s,
                              float* r_out, float* g_out, float* b_out) {
  const float yn = (y - s->y_offset_in) * s->y_in_scale;
  const float un = (u - 512) * s->uv_in_scale;
  const float vn = (v - 512) * s->uv_in_scale;
  *r_out = yn + s->cv * vn;
  *g_out = yn + s->cgu * un + s->cgv * vn;
  *b_out = yn + s->cu * un;
}

static inline int rgb_to_y10(float r, float g, float b, const ColorSpec* s) {
  const float yn = s->kr * r + s->kg * g + s->kb * b;
  return av_clip((int)lrintf(yn * s->y_out_scale) + s->y_offset_out, 0, 1023);
}

static inline void rgb_to_uv10(float r, float g, float b, const ColorSpec* s,
                               int* u_out, int* v_out) {
  const float yn = s->kr * r + s->kg * g + s->kb * b;
  const float un = (b - yn) / s->cu;
  const float vn = (r - yn) / s->cv;
  *u_out = av_clip((int)lrintf(un * s->uv_out_scale) + 512, 0, 1023);
  *v_out = av_clip((int)lrintf(vn * s->uv_out_scale) + 512, 0, 1023);
}

typedef struct {
  AVFrame* dst;
  const ProxyContext* pc;
  ColorSpec spec;
  int sub_x;
  int sub_y;
  int chroma_w;
  int chroma_h;
  float inv_n;
} ComposeJob;

// Composite a slice of chroma rows in [slice_start, slice_end). The composite
// is in non-linear RGB (matches cairo's sRGB-encoded output and what
// vf_overlay does), so subtitle/logo edges look identical to the previous
// split/overlay pipeline.
static int composite_slice(AVFilterContext* ctx, void* arg, int jobnr,
                           int nb_jobs) {
  const ComposeJob* j = arg;
  const int slice_start = (j->chroma_h * jobnr) / nb_jobs;
  const int slice_end = (j->chroma_h * (jobnr + 1)) / nb_jobs;

  const int sub_x = j->sub_x;
  const int sub_y = j->sub_y;
  const float inv_n = j->inv_n;
  const ColorSpec* spec = &j->spec;
  const ProxyContext* pc = j->pc;
  AVFrame* dst = j->dst;

  uint8_t* y_plane = dst->data[0];
  uint8_t* u_plane = dst->data[1];
  uint8_t* v_plane = dst->data[2];
  const int y_stride = dst->linesize[0];
  const int u_stride = dst->linesize[1];
  const int v_stride = dst->linesize[2];

  for (int cy = slice_start; cy < slice_end; cy++) {
    for (int cx = 0; cx < j->chroma_w; cx++) {
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
  return 0;
}

static void composite_bgra_on_yuv(AVFilterContext* ctx, AVFrame* dst,
                                  const ProxyContext* pc) {
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(dst->format);
  ComposeJob job = {
      .dst = dst,
      .pc = pc,
      .spec = color_spec_for_frame(dst),
      .sub_x = 1 << desc->log2_chroma_w,
      .sub_y = 1 << desc->log2_chroma_h,
  };
  job.chroma_w = dst->width / job.sub_x;
  job.chroma_h = dst->height / job.sub_y;
  job.inv_n = 1.0f / (float)(job.sub_x * job.sub_y);

  const int nb = FFMIN(job.chroma_h, ff_filter_get_nb_threads(ctx));
  ff_filter_execute(ctx, composite_slice, &job, NULL, FFMAX(nb, 1));
}

static int ensure_scratch(ProxyContext* pc, int w, int h) {
  const int linesize = w * 4;
  const int size = linesize * h;
  if (size == pc->scratch_size && linesize == pc->scratch_linesize) {
    return 0;
  }
  av_freep(&pc->scratch);
  pc->scratch = av_mallocz(size);
  if (!pc->scratch) {
    pc->scratch_size = 0;
    pc->scratch_linesize = 0;
    return AVERROR(ENOMEM);
  }
  pc->scratch_linesize = linesize;
  pc->scratch_size = size;
  pc->cache_present = 0;  // resize invalidates the previous render
  return 0;
}

static int filter_frame(AVFilterLink* inlink, AVFrame* in) {
  AVFilterContext* ctx = inlink->dst;
  AVFilterLink* outlink = ctx->outputs[0];
  ProxyContext* pc = ctx->priv;

  int rc = ensure_scratch(pc, in->width, in->height);
  if (rc < 0) {
    av_frame_free(&in);
    return rc;
  }

  const double time_ms = in->pts * av_q2d(inlink->time_base) * 1000;

  uint64_t version = 0;
  int hit = 0;
  if (pc->filter_version) {
    version = pc->filter_version(time_ms, pc->user_data);
    hit = pc->cache_present && version == pc->cached_version;
  }

  if (!hit) {
    memset(pc->scratch, 0, pc->scratch_size);
    rc = pc->filter_frame(pc->scratch, pc->scratch_size,
                          in->width, in->height,
                          pc->scratch_linesize, time_ms,
                          pc->user_data);
    if (rc != 0) {
      av_log(ctx, AV_LOG_ERROR, "filter_frame returned: %d\n", rc);
      av_frame_free(&in);
      return AVERROR_UNKNOWN;
    }
    pc->cached_version = version;
    pc->cache_present = 1;
  }

  composite_bgra_on_yuv(ctx, in, pc);

  return ff_filter_frame(outlink, in);
}

static int config_input(AVFilterLink* inlink) {
  return ensure_scratch(inlink->dst->priv, inlink->w, inlink->h);
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
    .p.flags = AVFILTER_FLAG_SLICE_THREADS,
    .priv_size = sizeof(ProxyContext),
    .init = init,
    .uninit = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_PIXFMTS(AV_PIX_FMT_YUV420P10LE,
                   AV_PIX_FMT_YUV422P10LE,
                   AV_PIX_FMT_YUV444P10LE),
};
