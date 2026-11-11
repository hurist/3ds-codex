#ifndef PICTURES_H
#define PICTURES_H

#include "ui.h"

enum { PICTURE_COUNT = 16, THUMB_WIDTH = 56, THUMB_HEIGHT = 36, PREVIEW_WIDTH = 800, PREVIEW_HEIGHT = 384 };

typedef struct {
    unsigned offset;
    char token[33];
    unsigned width;
    unsigned height;
    u8 pixels[THUMB_WIDTH * THUMB_HEIGHT * 2];
} PictureThumb;

typedef struct {
    PictureThumb thumbs[PICTURE_COUNT];
    unsigned count;
    unsigned selected;
    bool open;
    bool ready;
    unsigned width;
    unsigned height;
    unsigned zoom;
    int pan_x;
    int pan_y;
    int drag_x;
    int drag_y;
    bool dragging;
    char error[128];
    u8 pixels[PREVIEW_WIDTH * PREVIEW_HEIGHT * 2];
} Pictures;

void pictures_load_thumbs(Pictures *pictures, const char *index, const char *body);
int pictures_at(const Pictures *pictures, unsigned offset);
bool pictures_load(Pictures *pictures, const char *response, size_t length);
void pictures_prepare(Pictures *pictures, unsigned selected);
void pictures_draw(const Pictures *pictures, Screen top, Screen bottom);
void pictures_draw_thumb(const PictureThumb *thumb, Screen screen, int x, int y, unsigned slice);
bool pictures_input(Pictures *pictures, u32 pressed, u32 held, touchPosition touch, int *change);

#endif
