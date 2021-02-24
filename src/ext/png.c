// MIT License

// Copyright (c) 2021 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "png.h"

#include <string.h>
#include <stdlib.h>
#include <png.h>

typedef struct
{
    png_buffer buffer;
    size_t pos;
} PngStream;

static void pngReadCallback(png_structp png, png_bytep out, png_size_t size)
{
    PngStream* stream = png_get_io_ptr(png);
    memcpy(out, stream->buffer.data + stream->pos, size);
    stream->pos += size;
}

png_img png_read(png_buffer buf)
{
    png_img res = { 0 };

    if (png_sig_cmp(buf.data, 0, 8) == 0)
    {
        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop info = png_create_info_struct(png);

        PngStream stream = { .buffer = buf};

        png_set_read_fn(png, &stream, pngReadCallback);
        png_read_info(png, info);

        res.width = png_get_image_width(png, info);
        res.height = png_get_image_height(png, info);
        s32 colorType = png_get_color_type(png, info);
        s32 bitDepth = png_get_bit_depth(png, info);

        if (bitDepth == 16)
            png_set_strip_16(png);

        if (colorType == PNG_COLOR_TYPE_PALETTE)
            png_set_palette_to_rgb(png);

        // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
        if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
            png_set_expand_gray_1_2_4_to_8(png);

        if (png_get_valid(png, info, PNG_INFO_tRNS))
            png_set_tRNS_to_alpha(png);

        // These colorType don't have an alpha channel then fill it with 0xff.
        if (colorType == PNG_COLOR_TYPE_RGB ||
            colorType == PNG_COLOR_TYPE_GRAY ||
            colorType == PNG_COLOR_TYPE_PALETTE)
            png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

        if (colorType == PNG_COLOR_TYPE_GRAY ||
            colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
            png_set_gray_to_rgb(png);

        png_read_update_info(png, info);

        res.data = malloc(sizeof(u32) * res.width * res.height);
        png_bytep* rows = (png_bytep*)malloc(sizeof(png_bytep) * res.height);

        for (s32 i = 0; i < res.height; i++)
            rows[i] = res.data + res.width * i * sizeof(u32);

        png_read_image(png, rows);

        free(rows);

        png_destroy_read_struct(&png, &info, NULL);
    }

    return res;
}

static void pngWriteCallback(png_structp png, png_bytep data, png_size_t size)
{
    PngStream* stream = png_get_io_ptr(png);

    stream->buffer.size += (u32)size;

    stream->buffer.data = realloc(stream->buffer.data, stream->buffer.size);
    memcpy(stream->buffer.data + stream->pos, data, size);
    stream->pos += size;
}

static void pngFlushCallback(png_structp png) {}

png_buffer png_write(png_img src)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);

    PngStream stream = {0};
    png_set_write_fn(png, &stream, pngWriteCallback, pngFlushCallback);

    // Output is 8bit depth, RGBA format.
    png_set_IHDR(
        png,
        info,
        src.width, src.height,
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    png_write_info(png, info);

    png_bytep* rows = malloc(sizeof(png_bytep) * src.height);
    for (s32 i = 0; i < src.height; i++)
        rows[i] = src.data + src.width * i * sizeof(u32);

    png_write_image(png, rows);
    png_write_end(png, NULL);

    png_destroy_write_struct(&png, &info);

    free(rows);

    return stream.buffer;
}