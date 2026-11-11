#include "display.h"

#include <string.h>

extern const u8 glyphs[];

static u32 next_character(const char **cursor)
{
    const u8 *p = (const u8 *)*cursor;
    u32 code = *p++;
    int remaining = 0;
    if (code >= 0xc2 && code <= 0xf4) {
        remaining = code < 0xe0 ? 1 : code < 0xf0 ? 2 : 3;
        code &= 0x7f >> (remaining + 1);
        while (remaining && (*p & 0xc0) == 0x80) {
            code = (code << 6) | (*p++ & 0x3f);
            remaining--;
        }
    } else if (code >= 0x80) {
        code = '?';
    }
    *cursor = (const char *)p;
    return remaining ? '?' : code;
}

static int character_width(u32 code)
{
    return code == '\t' ? 32 : code <= 0x024f ? 8 : 16;
}

static const u8 *character_bitmap(u32 code)
{
    static const struct {u32 first, last;} ranges[] = {
        {0x0020, 0x007e}, {0x00a0, 0x024f}, {0x2000, 0x206f}, {0x2190, 0x2193},
        {0x3000, 0x303f}, {0x3400, 0x4dbf}, {0x4e00, 0x9fff},
        {0xff00, 0xffef},
    };
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        if (code >= ranges[i].first && code <= ranges[i].last) {
            return glyphs + (offset + code - ranges[i].first) * 64;
        }
        offset += ranges[i].last - ranges[i].first + 1;
    }
    return glyphs + ('?' - ' ') * 64;
}

Screen screen_back_buffer(gfxScreen_t side)
{
    Screen result;
    result.pixels = gfxGetFramebuffer(side, GFX_LEFT, &result.height, &result.width);
    return result;
}

static void pixel(Screen screen, int x, int y, u32 rgb)
{
    u8 *address = screen.pixels + (x * screen.height + screen.height - 1 - y) * 3;
    address[0] = (u8)rgb;
    address[1] = (u8)(rgb >> 8);
    address[2] = (u8)(rgb >> 16);
}

void fill(Screen screen, int x, int y, int width, int height, u32 rgb)
{
    int right = x + width < screen.width ? x + width : screen.width;
    int bottom = y + height < screen.height ? y + height : screen.height;
    for (int column = x < 0 ? 0 : x; column < right; column++) {
        for (int row = y < 0 ? 0 : y; row < bottom; row++) {
            pixel(screen, column, row, rgb);
        }
    }
}

static void blend(Screen screen, int x, int y, u32 rgb, unsigned alpha)
{
    u8 *address = screen.pixels + (x * screen.height + screen.height - 1 - y) * 3;
    for (unsigned component = 0; component < 3; component++) {
        unsigned source = (u8)(rgb >> (8 * component));
        address[component] = (source * alpha + address[component] * (3 - alpha) + 1) / 3;
    }
}

void text_scaled(Screen screen, int x, int y, const char *start, const char *end, u32 rgb, int size)
{
    const char *cursor = start;
    while (cursor < end && x < screen.width) {
        u32 code = next_character(&cursor);
        int width = character_width(code) * size / 16;
        if (code == '\t') {
            x += width;
            continue;
        }
        const u8 *bitmap = character_bitmap(code);
        for (int row = 0; row < size && y + row < screen.height; row++) {
            int source_row = row * 16 / size;
            for (int column = 0; column < width && x + column < screen.width; column++) {
                int source_column = column * 16 / size;
                unsigned coverage = (bitmap[source_row * 4 + source_column / 4] >>
                                     (6 - source_column % 4 * 2)) & 3;
                if (coverage) {
                    blend(screen, x + column, y + row, rgb, coverage);
                }
            }
        }
        x += width;
    }
}

void text(Screen screen, int x, int y, const char *start, const char *end, u32 rgb)
{
    text_scaled(screen, x, y, start, end, rgb, 16);
}

const char *line_end(const char *start, int width)
{
    const char *cursor = start;
    while (*cursor && *cursor != '\n') {
        const char *previous = cursor;
        width -= character_width(next_character(&cursor));
        if (width < 0) {
            return previous;
        }
    }
    return cursor;
}


int text_width(const char *start, const char *end)
{
    int width = 0;
    while (start < end) {
        width += character_width(next_character(&start));
    }
    return width;
}

const char *text_next(const char *cursor)
{
    if (*cursor) {
        next_character(&cursor);
    }
    return cursor;
}

const char *text_previous(const char *start, const char *cursor)
{
    if (cursor > start) {
        do {
            cursor--;
        } while (cursor > start && ((unsigned char)*cursor & 0xc0) == 0x80);
    }
    return cursor;
}
