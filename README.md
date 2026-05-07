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

### Optional caching hint

A plugin may export an additional symbol:

- `uint64_t filter_version(double ts_millis, void *user_data)`

If present, the proxy calls it before each frame. When the returned value
matches the value from the previous render the proxy reuses the cached
scratch buffer and skips both the `memset` and the `filter_frame` call.
The plugin must compute the value as a pure function of `ts_millis` and
its init-time state (no internal "last seen" tracking) so backward seeks
work correctly. Plugins that don't export this symbol render every frame
as before.

## Pixel formats

The filter accepts 10-bit YUV input only:

- `AV_PIX_FMT_YUV420P10LE`
- `AV_PIX_FMT_YUV422P10LE`
- `AV_PIX_FMT_YUV444P10LE`

The proxied filter still paints into an 8-bit premultiplied BGRA scratch
buffer (cairo's natural format); the proxy filter composites that scratch
onto the 10-bit YUV frame in-place. No `split`, `overlay`, or
`unpremultiply` are needed in the user filter graph:

`-filter_complex "proxy=<params>"`

Colorspace and range are taken from the input frame metadata (BT.709 /
limited by default, BT.601 if the frame is tagged as such, full range if
tagged JPEG).

## License

Copyright 2020 Sveriges Television AB.

This software is released under the GNU Lesser General Public License
version 2.1 or later (LGPL v2.1+).

## Credits

Originally created by Christer Sandberg <https://github.com/chrsan>.

[1]: https://www.ffmpeg.org
