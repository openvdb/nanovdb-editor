// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*
 * bmp2png: convert a 24/32-bpp uncompressed BMP to an uncompressed PNG.
 * No external dependencies. The PNG is uncompressed in the sense that the
 * deflate stream uses only stored blocks (BTYPE=00); it is still wrapped in
 * the mandatory zlib container with adler32, as required by the PNG spec.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vector>

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static void make_crc_table(void)
{
    for (uint32_t n = 0; n < 256; n++)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len)
{
    if (!crc_table_ready)
        make_crc_table();
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static uint32_t adler32_of(const uint8_t* buf, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++)
    {
        a = (a + buf[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void put_u32_be(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16_le(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t get_i32_le(const uint8_t* p)
{
    return (int32_t)get_u32_le(p);
}

static void write_chunk(std::vector<uint8_t>& f, const char* type, const uint8_t* data, size_t len)
{
    uint8_t hdr[8];
    put_u32_be(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    f.insert(f.end(), hdr, hdr + 8);
    if (len)
        f.insert(f.end(), data, data + len);

    uint32_t crc = 0xffffffffu;
    crc = crc32_update(crc, (const uint8_t*)type, 4);
    if (len)
        crc = crc32_update(crc, data, len);
    crc ^= 0xffffffffu;

    uint8_t tail[4];
    put_u32_be(tail, crc);
    f.insert(f.end(), tail, tail + 4);
}

static void raw_image_to_png(
    std::vector<uint8_t>& f, const uint8_t* input_data, int32_t width, int32_t height, bool force_opaque)
{
    /* Build the PNG raw scanline buffer: per row, one filter byte (0=None)
       followed by RGB(A) samples, top to bottom. */
    size_t row_len = 1u + (size_t)width * 4u;
    size_t raw_len = row_len * (size_t)height;
    uint8_t* raw = (uint8_t*)malloc(raw_len);

    for (int32_t y = 0; y < height; y++)
    {
        const uint8_t* src = input_data + (size_t)y * width * 4u;
        uint8_t* dst = raw + (size_t)y * row_len;
        *dst++ = 0;
        for (int32_t x = 0; x < width; x++)
        {
            const uint8_t* p = src + (size_t)x * 4u;
            dst[0] = p[0];
            dst[1] = p[1];
            dst[2] = p[2];
            dst[3] = force_opaque ? 255 : p[3];
            dst += 4;
        }
    }

    /* Wrap raw bytes in a zlib stream made of stored deflate blocks. */
    const size_t max_block = 65535u;
    size_t num_blocks = raw_len == 0 ? 1 : (raw_len + max_block - 1) / max_block;
    size_t zlib_cap = 2u + num_blocks * 5u + raw_len + 4u;
    uint8_t* zlib = (uint8_t*)malloc(zlib_cap);
    size_t zp = 0;
    zlib[zp++] = 0x78; /* CMF: deflate, 32K window */
    zlib[zp++] = 0x01; /* FLG: FLEVEL=0, FCHECK so (CMF*256+FLG)%31==0 */

    size_t pos = 0;
    do
    {
        size_t remaining = raw_len - pos;
        size_t block = remaining > max_block ? max_block : remaining;
        int final_block = (pos + block >= raw_len);
        zlib[zp++] = (uint8_t)(final_block ? 0x01 : 0x00);
        zlib[zp++] = (uint8_t)(block & 0xff);
        zlib[zp++] = (uint8_t)((block >> 8) & 0xff);
        uint16_t nlen = (uint16_t) ~(uint16_t)block;
        zlib[zp++] = (uint8_t)(nlen & 0xff);
        zlib[zp++] = (uint8_t)((nlen >> 8) & 0xff);
        if (block)
            memcpy(zlib + zp, raw + pos, block);
        zp += block;
        pos += block;
    } while (pos < raw_len);

    uint32_t adler = adler32_of(raw, raw_len);
    put_u32_be(zlib + zp, adler);
    zp += 4;

    /* Write the PNG. */
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    f.insert(f.end(), sig, sig + 8);

    uint8_t ihdr[13];
    put_u32_be(ihdr, (uint32_t)width);
    put_u32_be(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* color type: RGBA or RGB */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter method */
    ihdr[12] = 0; /* interlace: none */
    write_chunk(f, "IHDR", ihdr, 13);
    write_chunk(f, "IDAT", zlib, zp);
    write_chunk(f, "IEND", NULL, 0);

    free(raw);
    free(zlib);
}
