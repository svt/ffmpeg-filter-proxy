/*
 * SPDX-FileCopyrightText: 2020 Sveriges Television AB
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <dlfcn.h>
#include <string.h>

#include "avfilter.h"
#include "formats.h"
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
  int clear;
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

  if (pc->handle) {
    if (pc->filter_uninit) {
      pc->filter_uninit(pc->user_data);
    }

    dlclose(pc->handle);
  }
}

static void clear_image(AVFrame* out) {
  for (int i = 0; i < out->height; i++) {
    for (int j = 0; j < out->width; j++) {
      AV_WN32(out->data[0] + i * out->linesize[0] + j * 4, 0);
    }
  }
}

static int filter_frame(AVFilterLink* inlink, AVFrame* in) {
  AVFilterContext* ctx = inlink->dst;
  AVFilterLink* outlink = ctx->outputs[0];

  ProxyContext* pc = ctx->priv;

  AVFrame* out;
  int direct = 0;
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

  av_assert0(in->format != -1);

  int data_size =
      av_image_get_buffer_size(in->format, in->width, in->height, 1);
  if (data_size < 0) {
    av_log(ctx, AV_LOG_ERROR, "error getting buffer size\n");
    return data_size;
  }

  if (pc->clear) {
    clear_image(out);
  }

  double time_ms = in->pts * av_q2d(inlink->time_base) * 1000;

  int rc = pc->filter_frame(out->data[0], data_size, in->width, in->height,
                            in->linesize[0], time_ms, pc->user_data);

  if (!direct) {
    av_frame_free(&in);
  }

  if (rc != 0) {
    av_log(ctx, AV_LOG_ERROR, "filter_frame returned: %d\n", rc);
    return AVERROR_UNKNOWN;
  }

  return ff_filter_frame(outlink, out);
}

static int config_input(AVFilterLink* inlink) {
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
    {"clear",
     "clear frame before filtering",
     OFFSET(clear),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {NULL},
};

AVFILTER_DEFINE_CLASS(proxy);

const FFFilter ff_vf_proxy = {
    .p.name = "proxy",
    .p.description = NULL_IF_CONFIG_SMALL("Video filter proxy."),
    .priv_size = sizeof(ProxyContext),
    .init = init,
    .uninit = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_BGRA),
    .p.priv_class = &proxy_class,
};
