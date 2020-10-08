/*
 * SPDX-FileCopyrightText: 2020 Sveriges Television AB
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <dlfcn.h>
#include <string.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "video.h"

typedef struct {
  const AVClass* class;
  char* filter_path;
  char* config;
  int split;
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
} ProxyContext;

#define OFFSET(x) offsetof(ProxyContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static int config_output_bgra(AVFilterLink* outlink) {
  AVFilterContext* ctx = outlink->src;
  outlink->w = ctx->inputs[0]->w;
  outlink->h = ctx->inputs[0]->h;
  outlink->time_base = ctx->inputs[0]->time_base;
  outlink->sample_aspect_ratio = ctx->inputs[0]->sample_aspect_ratio;
  outlink->format = AV_PIX_FMT_BGRA;
  return 0;
}

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

  int nb_out, ret, i;
  nb_out = pc->split ? 2 : 1;
  for (i = 0; i < nb_out; i++) {
    AVFilterPad pad = {0};
    pad.type = AVMEDIA_TYPE_VIDEO;
    pad.name = av_asprintf("output%d", i);
    if (!pad.name) {
      return AVERROR(ENOMEM);
    }

    if (i == 1) {
      pad.config_props = config_output_bgra;
    }

    if ((ret = ff_insert_outpad(ctx, i, &pad)) < 0) {
      av_freep(&pad.name);
      return ret;
    }
  }
  return 0;
}

static av_cold void uninit(AVFilterContext* ctx) {
  ProxyContext* pc = ctx->priv;

  if (pc->handle) {
    if (pc->filter_uninit) {
      pc->filter_uninit(pc->user_data);
    }

    dlclose(pc->handle);
  }
}

static int query_formats(AVFilterContext* ctx) {
  ProxyContext* pc = ctx->priv;

  if (pc->split) {
    return ff_set_common_formats(ctx, ff_all_formats(AVMEDIA_TYPE_VIDEO));
  }

  const enum AVPixelFormat pix_fmts[] = {
      AV_PIX_FMT_BGRA,
      AV_PIX_FMT_NONE,
  };

  AVFilterFormats* fmts_list = ff_make_format_list(pix_fmts);
  if (!fmts_list) {
    return AVERROR(ENOMEM);
  }

  return ff_set_common_formats(ctx, fmts_list);
}

static void clear_image(AVFrame* out) {
  for (int i = 0; i < out->height; i++)
    for (int j = 0; j < out->width; j++)
      AV_WN32(out->data[0] + i * out->linesize[0] + j * 4, 0);
}

static int do_filter(AVFilterLink* inlink, AVFrame* in, AVFrame* out) {
  AVFilterContext* ctx = inlink->dst;
  ProxyContext* pc = ctx->priv;
  int data_size =
      av_image_get_buffer_size(out->format, out->width, out->height, 1);
  if (data_size < 0) {
    av_log(ctx, AV_LOG_ERROR, "error getting buffer size\n");
    return data_size;
  }
  double time_ms = in->pts * av_q2d(inlink->time_base) * 1000;

  int rc = pc->filter_frame(out->data[0], data_size, out->width, out->height,
                            out->linesize[0], time_ms, pc->user_data);

  if (rc != 0) {
    av_log(ctx, AV_LOG_ERROR, "filter_frame returned: %d\n", rc);
    return AVERROR_UNKNOWN;
  }
  return 0;
}

static int filter_frame_split(AVFilterLink* inlink, AVFrame* in) {
  AVFilterContext* ctx = inlink->dst;
  AVFilterLink* mainlink = ctx->outputs[0];
  AVFilterLink* overlaylink = ctx->outputs[1];
  ProxyContext* pc = ctx->priv;
  AVFrame* out =
      ff_get_video_buffer(overlaylink, overlaylink->w, overlaylink->h);

  if (!out) {
    av_log(ctx, AV_LOG_ERROR, "error ff_get_video_buffer\n");
    av_frame_free(&in);
    return AVERROR(ENOMEM);
  }

  clear_image(out);
  out->pts = in->pts;

  int ret;

  if ((ret = do_filter(inlink, in, out)) < 0 ||
      (ret = ff_filter_frame(mainlink, in)) < 0) {
    av_frame_free(&out);
    return ret;
  }
  return ff_filter_frame(overlaylink, out);
}

static int filter_frame(AVFilterLink* inlink, AVFrame* in) {
  AVFilterContext* ctx = inlink->dst;
  ProxyContext* pc = ctx->priv;

  if (pc->split) {
    return filter_frame_split(inlink, in);
  }
  AVFilterLink* outlink = ctx->outputs[0];

  av_assert0(in->format != -1);

  AVFrame* out;
  int direct = 0;
  int ret;

  if (av_frame_is_writable(in)) {
    direct = 1;
    out = in;
  } else {
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
      av_frame_free(&in);
      return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out, in);
  }

  ret = do_filter(inlink, in, out);
  if (!direct) {
    av_frame_free(&in);
  }
  if (ret < 0) {
    return ret;
  }
  return ff_filter_frame(outlink, out);
}

static const AVFilterPad inputs[] = {{.name = "default",
                                      .type = AVMEDIA_TYPE_VIDEO,
                                      .filter_frame = filter_frame},
                                     {NULL}};

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
    {"split",
     "split output to a unmodified and an overlay frame",
     OFFSET(split),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {NULL},
};

AVFILTER_DEFINE_CLASS(proxy);

AVFilter ff_vf_proxy = {
    .name = "proxy",
    .description = NULL_IF_CONFIG_SMALL("Video filter proxy."),
    .priv_size = sizeof(ProxyContext),
    .init = init,
    .uninit = uninit,
    .query_formats = query_formats,
    .inputs = inputs,
    .outputs = NULL,
    .flags = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .priv_class = &proxy_class,
};
