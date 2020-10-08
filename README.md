# ffmpeg-filter-proxy

A [FFMpeg][1] video filter proxy.

## Purpose

- To make it easier to develop and test video filters for use in [FFmpeg][1].
- To make it easier to implement a video filter using different programming
  languages.

## Proxied filter

A dynamic shared object that provides the following signatures:

- `int filter_init(const char *config, void **user_data)`
- `int filter_frame(unsigned char *data, unsigned int data_size, int width, int height, int line_size, double ts_millis, void *user_data)`
- `void filter_uninit(void *user_data)`

On success, `filter_init` and `filter_frame` should return `0`.
A nonzero return value signals an error.

The `config` parameter to `filter_init` is filter implementation specific.
It could be a config filename or the complete config, or `NULL` if the proxied
filter doesn't need any specific configuration.

## Limitations

Only `AV_PIX_FMT_BGRA` is used right now since that's what we need.

However, there is a boolean option `split` that when enabled produces 2 outputs, 
the unmodified input frame with preserved pixel format and a transparent bgra frame passed through the proxied filter.

This can be used in combination with the overlay filter in Ffmpeg to preserve 10 bit color: 
`-vf "proxy=split=1:<other proxy params>,overlay=format=yuv420p10"`

## License

Copyright 2020 Sveriges Television AB.

This software is released under the GNU Lesser General Public License
version 2.1 or later (LGPL v2.1+).

## Primary Maintainers

Christer Sandberg <https://github.com/chrsan>

[1]: https://www.ffmpeg.org
