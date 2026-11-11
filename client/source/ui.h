#ifndef UI_H
#define UI_H

#include "display.h"
#include <stdbool.h>

enum {
    UI_BACKGROUND = 0x101921,
    UI_PANEL = 0x202e3a,
    UI_FOCUS = 0x2b5e59,
    UI_TEXT = 0xe6edf3,
    UI_MUTED = 0xa5b5c2,
    UI_ACCENT = 0x9de2d3,
    UI_WARNING = 0xffd28e,
    UI_DANGER = 0xffa7a3,
    UI_DARK_TEXT = 0x152c2a,
};

typedef struct { int x, y, w, h; } Rect;
typedef enum { UI_SECONDARY, UI_PRIMARY, UI_CAUTION, UI_DISABLED } ButtonTone;

void ui_clear(Screen screen);
void ui_card(Screen screen, Rect rect, u32 color, u32 outline);
void ui_label(Screen screen, int x, int y, int width, const char *value, u32 color);
void ui_title(Screen screen, const char *value, const char *detail, u32 detail_color);
void ui_button(Screen screen, Rect rect, const char *label, ButtonTone tone);
void ui_connection(Screen screen, const char *label, u32 color);
bool ui_hit(Rect rect, touchPosition touch);

#endif
