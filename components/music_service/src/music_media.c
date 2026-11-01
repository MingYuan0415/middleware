#define DBG_TAG "music_media"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_http.h"
#include "music_media.h"
#include "music_snapshot.h"
#include "music_login_protocol.h"

#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "png.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MUSIC_QR_MAX_BYTES (256U * 1024U)

static void *_psram_alloc(size_t size)
{
    return heap_caps_calloc(1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void _psram_free(void *pointer)
{
    heap_caps_free(pointer);
}

static void _music_image_destroy(void *payload)
{
    music_image_t *image = payload;
    if (image != NULL && image->pixels != NULL)
    {
        jpeg_free_align(image->pixels);
        image->pixels = NULL;
    }
}

static esp_err_t _decode_rgb565(const uint8_t *data, size_t data_size,
                                uint16_t requested_width,
                                uint16_t requested_height,
                                music_image_t **image_out)
{
    if (data == NULL || data_size == 0U || image_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    jpeg_dec_config_t config =
    {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .scale = { 0, 0 },
        .clipper = { 0, 0 },
        .rotate = JPEG_ROTATE_0D,
        .block_enable = false,
    };
    if (requested_width >= 8U && requested_height >= 8U)
    {
        /* The decoder requires scale dimensions to be multiples of 8. */
        config.scale.width = (uint16_t)(requested_width & 0xFFF8U);
        config.scale.height = (uint16_t)(requested_height & 0xFFF8U);
    }
    jpeg_dec_handle_t decoder = NULL;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    jpeg_dec_io_t io =
    {
        .inbuf = (uint8_t *)data,
        .inbuf_len = (int)data_size,
        .inbuf_remain = 0,
    };
    jpeg_dec_header_info_t header;
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK)
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (header.width == 0U || header.height == 0U ||
            header.width > 1024U || header.height > 1024U)
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    int output_size = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &output_size) != JPEG_ERR_OK ||
            output_size <= 0 || output_size > (2 * 1024 * 1024))
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t *pixels = jpeg_calloc_align((size_t)output_size, 16);
    if (pixels == NULL)
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_NO_MEM;
    }
    io.outbuf = pixels;
    io.out_size = output_size;
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK)
    {
        jpeg_free_align(pixels);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }
    jpeg_dec_close(decoder);
    music_image_t *image = music_snapshot_alloc(sizeof(*image),
        _music_image_destroy);
    if (image == NULL)
    {
        jpeg_free_align(pixels);
        return ESP_ERR_NO_MEM;
    }
    image->width = config.scale.width != 0U ? config.scale.width :
                   header.width;
    image->height = config.scale.height != 0U ? config.scale.height :
                    header.height;
    image->data_size = (size_t)output_size;
    image->pixels = pixels;
    image->token = 0U;
    *image_out = image;
    return ESP_OK;
}

static esp_err_t _decode_png_rgb565(const uint8_t *data, size_t data_size,
                                    music_image_t **image_out)
{
    if (data == NULL || data_size == 0U || image_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    png_image png;
    memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, data, data_size))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (png.width == 0U || png.height == 0U ||
            png.width > 1024U || png.height > 1024U ||
            (uint64_t)png.width * png.height > 1024U * 1024U)
    {
        png_image_free(&png);
        return ESP_ERR_INVALID_SIZE;
    }
    png.format = PNG_FORMAT_RGBA;
    const size_t rgba_size = PNG_IMAGE_SIZE(png);
    if (rgba_size == 0U || rgba_size > (2U * 1024U * 1024U))
    {
        png_image_free(&png);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *rgba = _psram_alloc(rgba_size);
    if (rgba == NULL)
    {
        png_image_free(&png);
        return ESP_ERR_NO_MEM;
    }
    if (!png_image_finish_read(&png, NULL, rgba, 0, NULL))
    {
        _psram_free(rgba);
        png_image_free(&png);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint16_t width = (uint16_t)png.width;
    const uint16_t height = (uint16_t)png.height;
    png_image_free(&png);

    const size_t pixel_bytes = (size_t)width * (size_t)height * 2U;
    uint8_t *pixels = jpeg_calloc_align(pixel_bytes, 16);
    if (pixels == NULL)
    {
        _psram_free(rgba);
        return ESP_ERR_NO_MEM;
    }
    uint16_t *output = (uint16_t *)pixels;
    for (size_t index = 0U; index < (size_t)width * height; ++index)
    {
        const uint32_t red = rgba[index * 4U];
        const uint32_t green = rgba[index * 4U + 1U];
        const uint32_t blue = rgba[index * 4U + 2U];
        const uint32_t alpha = rgba[index * 4U + 3U];
        const uint32_t inverse = 255U - alpha;
        const uint8_t mixed_red =
            (uint8_t)((red * alpha + 255U * inverse) / 255U);
        const uint8_t mixed_green =
            (uint8_t)((green * alpha + 255U * inverse) / 255U);
        const uint8_t mixed_blue =
            (uint8_t)((blue * alpha + 255U * inverse) / 255U);
        output[index] = (uint16_t)(((uint16_t)mixed_red & 0xF8U) << 8) |
                        (uint16_t)(((uint16_t)mixed_green & 0xFCU) << 3) |
                        (uint16_t)(mixed_blue >> 3);
    }
    _psram_free(rgba);

    music_image_t *image = music_snapshot_alloc(sizeof(*image),
        _music_image_destroy);
    if (image == NULL)
    {
        jpeg_free_align(pixels);
        return ESP_ERR_NO_MEM;
    }
    image->width = width;
    image->height = height;
    image->data_size = pixel_bytes;
    image->pixels = pixels;
    image->token = 0U;
    *image_out = image;
    return ESP_OK;
}

esp_err_t music_media_fetch_image(const char *url, uint16_t width,
                                  uint16_t height, uint32_t token,
                                  bool with_cookie, const char *referer,
                                  music_image_t **image)
{
    if (url == NULL || url[0] == '\0' || image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    music_http_result_t result;
    const esp_err_t error = music_http_get_referer(url, with_cookie, referer,
        &result, MUSIC_QR_MAX_BYTES);
    if (error != ESP_OK)
    {
        return error;
    }
    music_image_t *decoded = NULL;
    esp_err_t decode_result;
    if (music_login_png_signature((const uint8_t *)result.body,
                                  result.length))
    {
        decode_result = _decode_png_rgb565((const uint8_t *)result.body,
                                           result.length, &decoded);
    }
    else
    {
        decode_result = _decode_rgb565((const uint8_t *)result.body,
                                       result.length, width, height,
                                       &decoded);
    }
    music_http_result_free(&result);
    if (decode_result != ESP_OK)
    {
        return decode_result;
    }
    decoded->token = token;
    (void)snprintf(decoded->url, sizeof(decoded->url), "%s", url);
    *image = decoded;
    return ESP_OK;
}

esp_err_t music_net_fetch_image(const char *url, uint16_t width,
                                uint16_t height, uint32_t token,
                                music_image_t **image)
{
    return music_media_fetch_image(url, width, height, token, false, NULL,
                                   image);
}

