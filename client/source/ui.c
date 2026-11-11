#include "ui.h"

static void rounded(Screen screen, Rect rect, int radius, u32 color)
{
    for (int y = 0; y < rect.h; y++) {
        int edge = y < radius ? radius - y - 1 : y >= rect.h - radius ? y - (rect.h - radius) : 0;
        int inset = 0;
        while (inset < radius && (radius - inset) * (radius - inset) + edge * edge > radius * radius) {
            inset++;
        }
        fill(screen, rect.x + inset, rect.y + y, rect.w - 2 * inset, 1, color);
    }
}

void ui_clear(Screen screen)
{
    fill(screen, 0, 0, screen.width, screen.height, UI_BACKGROUND);
}

void ui_card(Screen screen, Rect rect, u32 color, u32 outline)
{
    if (outline) {
        rounded(screen, rect, 8, outline);
        rect = (Rect){rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2};
        rounded(screen, rect, 7, color);
    } else {
        rounded(screen, rect, 8, color);
    }
}

void ui_label(Screen screen, int x, int y, int width, const char *value, u32 color)
{
    if (width > screen.width - x) {
        width = screen.width - x;
    }
    if (width > 0) {
        text(screen, x, y, value, line_end(value, width), color);
    }
}

void ui_title(Screen screen, const char *value, const char *detail, u32 detail_color)
{
    int detail_width = detail ? text_width(detail, line_end(detail, 120)) : 0;
    int room = screen.width - 32 - (detail_width ? detail_width + 12 : 0);
    text_scaled(screen, 16, 15, value, line_end(value, room * 16 / 20), UI_TEXT, 20);
    if (detail_width) {
        ui_label(screen, screen.width - 16 - detail_width, 18, detail_width, detail, detail_color);
    }
}

void ui_button(Screen screen, Rect rect, const char *label, ButtonTone tone)
{
    u32 fill_color = tone == UI_PRIMARY ? UI_ACCENT : tone == UI_CAUTION ? UI_WARNING : UI_PANEL;
    u32 label_color = tone == UI_PRIMARY || tone == UI_CAUTION ? UI_DARK_TEXT :
                      tone == UI_DISABLED ? UI_MUTED : UI_TEXT;
    ui_card(screen, rect, fill_color, 0);
    ui_label(screen, rect.x + 10, rect.y + (rect.h - 16) / 2, rect.w - 18, label, label_color);
}

void ui_connection(Screen screen, const char *label, u32 color)
{
    rounded(screen, (Rect){16, 16, 10, 10}, 5, color);
    ui_label(screen, 32, 13, screen.width - 48, label, color);
}

bool ui_hit(Rect rect, touchPosition touch)
{
    return touch.px >= rect.x && touch.px < rect.x + rect.w &&
           touch.py >= rect.y && touch.py < rect.y + rect.h;
}
