#ifndef DISPLAY_H
#define DISPLAY_H

#include <3ds.h>

typedef struct {
    u8 *pixels;
    u16 width;
    u16 height;
} Screen;

Screen screen_init(gfxScreen_t side);
void fill(Screen screen, int x, int y, int width, int height, u32 rgb);
void text(Screen screen, int x, int y, const char *start, const char *end, u32 rgb);
void text_scaled(Screen screen, int x, int y, const char *start, const char *end, u32 rgb, int size);
const char *line_end(const char *start, int width);
int text_width(const char *start, const char *end);
const char *text_next(const char *cursor);
const char *text_previous(const char *start, const char *cursor);

#endif
