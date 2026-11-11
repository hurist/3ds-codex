#include "selection.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

enum {
    WIDTH = 296,
    VISIBLE = 6,
    ROW_HEIGHT = 18,
    TOUCH_TOP = 30,
};

unsigned selection_line(const SelectionPage *page, const char *cursor)
{
    unsigned low = 0;
    unsigned high = page->count;
    while (low + 1 < high) {
        unsigned mid = low + (high - low) / 2;
        if (page->lines[mid] <= cursor) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

static const char *column_at(const char *line, int column)
{
    const char *end = line_end(line, WIDTH);
    while (line < end) {
        const char *next = text_next(line);
        int width = text_width(line, next);
        if (column < width / 2) {
            break;
        }
        column -= width;
        line = next;
    }
    return line;
}

static void copy(Selection *selection, char clipboard[MESSAGE_CAPACITY],
                 const char *start, const char *end)
{
    size_t size = (size_t)(end - start);
    if (!size) {
        selection->notice = "请先选中一段文字";
    } else if (size >= MESSAGE_CAPACITY) {
        selection->notice = "选区太长，原剪贴板保留";
    } else {
        memcpy(clipboard, start, size);
        clipboard[size] = '\0';
        selection->notice = "已复制，可切换对话后粘贴";
    }
}

const char *selection_copy_message(const SelectionPage *page, const char *cursor,
                                   char clipboard[MESSAGE_CAPACITY])
{
    unsigned offset = (unsigned)(cursor - page->body);
    for (unsigned i = 0; i < page->message_count; i++) {
        MessageRange range = page->messages[i];
        if (offset >= range.label && offset <= range.end) {
            Selection result = {0};
            copy(&result, clipboard, page->body + range.body, page->body + range.end);
            return result.notice;
        }
    }
    return "请把光标移到一条完整消息";
}

bool selection_input(Selection *selection, const SelectionPage *page, u32 pressed, u32 repeat,
                     touchPosition touch, char clipboard[MESSAGE_CAPACITY])
{
    if (pressed & KEY_B) {
        return false;
    }

    unsigned row = selection_line(page, selection->cursor);
    if (repeat & KEY_LEFT) {
        selection->cursor = text_previous(page->body, selection->cursor);
    } else if (repeat & KEY_RIGHT) {
        selection->cursor = text_next(selection->cursor);
    } else if ((repeat & KEY_UP && row) || (repeat & KEY_DOWN && row + 1 < page->count)) {
        int column = text_width(page->lines[row], selection->cursor);
        unsigned destination = repeat & KEY_UP && row ? row - 1 : row + 1;
        selection->cursor = column_at(page->lines[destination], column);
    }
    if ((pressed & KEY_TOUCH) && touch.py >= TOUCH_TOP && touch.py < TOUCH_TOP + ROW_HEIGHT * VISIBLE) {
        unsigned target = selection->top + (touch.py - TOUCH_TOP) / ROW_HEIGHT;
        if (target < page->count) {
            selection->cursor = column_at(page->lines[target], (int)touch.px - 12);
        }
    }

    bool action_row = (pressed & KEY_TOUCH) && touch.py >= 150 && touch.py < 182;
    if ((pressed & KEY_X) || (action_row && touch.px < 105)) {
        selection->anchor = selection->cursor;
        selection->notice = "已设起点，移动光标选文字";
    } else if ((pressed & KEY_A) || (action_row && touch.px < 210)) {
        if (!selection->anchor) {
            selection->notice = "先按 X 设置选区起点";
        } else {
            const char *start = selection->anchor < selection->cursor ? selection->anchor : selection->cursor;
            const char *end = selection->anchor < selection->cursor ? selection->cursor : selection->anchor;
            copy(selection, clipboard, start, end);
        }
    } else if ((pressed & KEY_Y) || action_row) {
        selection->notice = selection_copy_message(page, selection->cursor, clipboard);
    }

    row = selection_line(page, selection->cursor);
    if (row < selection->top) {
        selection->top = row;
    } else if (row >= selection->top + VISIBLE) {
        selection->top = row - VISIBLE + 1;
    }
    return true;
}

static void line_text(Screen screen, int x, int y, const char *value, u32 color)
{
    text(screen, x, y, value, line_end(value, screen.width - x - 8), color);
}

static void draw_rows(Screen screen, int x, int y, const Selection *selection, const SelectionPage *page)
{
    const char *start = selection->anchor;
    const char *end = selection->cursor;
    if (start && start > end) {
        const char *swap = start;
        start = end;
        end = swap;
    }
    unsigned cursor_row = selection_line(page, selection->cursor);
    for (unsigned i = 0; i < VISIBLE && selection->top + i < page->count; i++) {
        unsigned row = selection->top + i;
        const char *line = page->lines[row];
        const char *limit = line_end(line, WIDTH);
        int line_y = y + ROW_HEIGHT * i;
        if (start && start < limit && end > line) {
            const char *left = start > line ? start : line;
            const char *right = end < limit ? end : limit;
            fill(screen, x + text_width(line, left), line_y, text_width(left, right), 16, UI_FOCUS);
        }
        text(screen, x, line_y, line, limit, UI_TEXT);
        if (row == cursor_row) {
            int caret = x + text_width(line, selection->cursor);
            fill(screen, caret, line_y, 2, 16, UI_ACCENT);
        }
    }
}

void selection_draw(Screen top, Screen bottom, const char *title, const Selection *selection,
                    const SelectionPage *page)
{
    ui_clear(top);
    ui_clear(bottom);
    char status[32] = "";
    if (selection->anchor && selection->anchor != selection->cursor) {
        const char *start = selection->anchor;
        const char *end = selection->cursor;
        if (start > end) {
            start = selection->cursor;
            end = selection->anchor;
        }
        unsigned count = 0;
        for (const char *cursor = start; cursor < end; cursor = text_next(cursor)) {
            count++;
        }
        snprintf(status, sizeof(status), "已选 %u 字", count);
    }
    ui_title(top, "选择文字", status, UI_ACCENT);
    ui_card(top, (Rect){16, 42, 368, 120}, UI_PANEL, 0);
    draw_rows(top, 26, 50, selection, page);
    line_text(bottom, 12, 6, title, UI_ACCENT);
    ui_card(bottom, (Rect){5, 25, 310, 116}, UI_PANEL, 0);
    draw_rows(bottom, 12, TOUCH_TOP, selection, page);
    ui_button(bottom, (Rect){5, 150, 98, 32}, "X 起点", UI_SECONDARY);
    ui_button(bottom, (Rect){110, 150, 98, 32}, "A 复制选区", UI_PRIMARY);
    ui_button(bottom, (Rect){215, 150, 100, 32}, "Y 复制整条", UI_SECONDARY);
    line_text(bottom, 12, 190, selection->notice, UI_ACCENT);
    line_text(bottom, 12, 218, "B 返回对话", UI_TEXT);
}
