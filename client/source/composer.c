#include "composer.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const Rect toolbar_left = {5, 34, 99, 31};
static const Rect toolbar_middle = {110, 34, 99, 31};
static const Rect toolbar_right = {215, 34, 100, 31};
static const Rect previous_words = {5, 34, 21, 31};
static const Rect next_words = {294, 34, 21, 31};
static const Rect symbols_button = {5, 195, 61, 36};
static const Rect language_button = {72, 195, 52, 36};
static const Rect space_button = {130, 195, 110, 36};
static const Rect english_comma = {130, 195, 30, 36};
static const Rect english_space = {166, 195, 38, 36};
static const Rect english_period = {210, 195, 30, 36};
static const Rect send_button = {246, 195, 69, 36};

static const char *chinese_keys[] = {
    "，", "2 ABC", "3 DEF", "删除",
    "4 GHI", "5 JKL", "6 MNO", "。",
    "7 PQRS", "8 TUV", "9 WXYZ", "？",
};
static const char *english_rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *symbol_pages[][32] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "，", "。", "？", "！", "、", "；",
     "：", "（", "）", "「", "」", "《", "》", "【", "】", "“", "”", "‘", "’", "…", "—", "·"},
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", ".", ",", "?", "!", ":", ";",
     "'", "\"", "-", "_", "/", "\\", "@", "#", "(", ")", "[", "]", "{", "}", "=", "+"},
    {"<", ">", "|", "&", "%", "*", "^", "~", "`", "$", "@", "#", "_", "-", "+", "=",
     "(", ")", "[", "]", "{", "}", "/", "\\", ":", ";", "'", "\"", ".", ",", "?", "!"},
};
static const char *symbol_tabs[] = {"中文符号", "英文符号", "代码符号"};
static const Rect symbol_tab_rects[] = {
    {5, 34, 99, 31}, {110, 34, 99, 31}, {215, 34, 100, 31},
};

typedef enum { KEY_LITERAL, KEY_DIGIT, KEY_ERASE, KEY_SHIFT } KeyboardAction;

typedef struct {
    Rect rect;
    char label[12];
    KeyboardAction action;
} KeyboardKey;

static void write_text(Screen screen, int x, int y, const char *value, u32 color)
{
    text(screen, x, y, value, line_end(value, screen.width - x - 5), color);
}

static bool has_text(const Composer *draft)
{
    for (unsigned i = 0; i < draft->length; i++) {
        if (!isspace((unsigned char)draft->text[i])) {
            return true;
        }
    }
    return false;
}

bool composer_insert_text(Composer *draft, const char *value)
{
    size_t size = strlen(value);
    if (draft->length + size >= sizeof(draft->text)) {
        return false;
    }
    memmove(draft->text + draft->caret + size, draft->text + draft->caret,
            draft->length - draft->caret + 1);
    memcpy(draft->text + draft->caret, value, size);
    draft->caret += size;
    draft->length += size;
    return true;
}

static bool commit_candidate(Composer *draft, unsigned index, bool require_all)
{
    PinyinPage page = pinyin_candidates(draft->digits, draft->candidate_page);
    const char *word = pinyin_word(&page, index);
    PinyinCandidate item = page.items[index];
    unsigned length = strlen(draft->digits);
    unsigned consumed = item.consumed;
    bool typed_match = item.kind == PINYIN_EXACT || item.kind == PINYIN_COMBINED;
    if (!word || (require_all && !typed_match) || !composer_insert_text(draft, word)) return false;
    memmove(draft->digits, draft->digits + consumed, length - consumed + 1);
    draft->candidate_page = 0;
    return true;
}

static void erase(Composer *draft)
{
    size_t length = strlen(draft->digits);
    if (length) {
        draft->digits[length - 1] = '\0';
        draft->candidate_page = 0;
    } else if (draft->caret) {
        unsigned old_caret = draft->caret;
        draft->caret = text_previous(draft->text, draft->text + old_caret) - draft->text;
        memmove(draft->text + draft->caret, draft->text + old_caret, draft->length - old_caret + 1);
        draft->length -= old_caret - draft->caret;
    }
}

static void space_or_commit(Composer *draft)
{
    if (*draft->digits) {
        commit_candidate(draft, 0, false);
    } else {
        composer_insert_text(draft, " ");
    }
}

static unsigned keyboard_rows(const Composer *draft)
{
    return draft->symbols ? 4 : 3;
}

static unsigned keyboard_columns(const Composer *draft, unsigned row)
{
    if (draft->symbols) return 8;
    if (!draft->latin) return 4;
    return row == 0 ? 10 : 9;
}

static KeyboardKey keyboard_key(const Composer *draft, unsigned row, unsigned column)
{
    KeyboardKey key = {0};
    if (draft->symbols) {
        key.rect = (Rect){5 + 39 * column, 70 + 30 * row, 36, 27};
        snprintf(key.label, sizeof(key.label), "%s", symbol_pages[draft->symbol_page][row * 8 + column]);
    } else if (!draft->latin) {
        key.rect = (Rect){5 + 78 * column, 70 + 40 * row, 74, 36};
        snprintf(key.label, sizeof(key.label), "%s", chinese_keys[row * 4 + column]);
        if (key.label[0] >= '2' && key.label[0] <= '9') key.action = KEY_DIGIT;
        if (row == 0 && column == 3) key.action = KEY_ERASE;
    } else {
        key.rect = (Rect){5 + (row ? 15 : 0) + 31 * column, 70 + 40 * row, 28, 36};
        if (row == 2 && (column == 0 || column == 8)) {
            key.action = column == 0 ? KEY_SHIFT : KEY_ERASE;
            snprintf(key.label, sizeof(key.label), "%s", column == 0 ? "↑" : "删");
        } else {
            char letter = english_rows[row][row == 2 ? column - 1 : column];
            key.label[0] = draft->shift ? toupper((unsigned char)letter) : letter;
        }
    }
    return key;
}

static bool finish_pinyin(Composer *draft)
{
    if (!*draft->digits || commit_candidate(draft, 0, true)) return true;
    draft->notice = "请先选词或删掉未完成的拼音";
    return false;
}

static void input_key(Composer *draft, KeyboardKey key)
{
    switch (key.action) {
        case KEY_ERASE:
            erase(draft);
            break;
        case KEY_SHIFT:
            draft->shift = !draft->shift;
            break;
        case KEY_DIGIT: {
            size_t length = strlen(draft->digits);
            if (length + 1 < sizeof(draft->digits)) {
                draft->digits[length] = key.label[0];
                draft->digits[length + 1] = '\0';
                draft->candidate_page = 0;
            }
            break;
        }
        case KEY_LITERAL:
            if (finish_pinyin(draft)) composer_insert_text(draft, key.label);
            break;
    }
}

static Rect candidate_rect(unsigned index)
{
    return (Rect){29 + 66 * index, 34, 64, 31};
}

static void change_language(Composer *draft)
{
    if (finish_pinyin(draft)) {
        draft->latin = !draft->latin;
        draft->symbols = false;
        draft->row = draft->column = 0;
    }
}

static void toggle_symbols(Composer *draft)
{
    draft->symbols = !draft->symbols;
    if (draft->symbols) draft->symbol_page = draft->latin ? 1 : 0;
    draft->row = draft->column = 0;
}

static void paste(Composer *draft, const char *clipboard)
{
    if (!*clipboard) {
        draft->notice = "剪贴板是空的";
    } else if (composer_insert_text(draft, clipboard)) {
        draft->notice = "已粘贴到光标位置";
    } else {
        draft->notice = "超过草稿容量，未粘贴";
    }
}

ComposeAction composer_input(Composer *draft, u32 pressed, u32 repeat, const char *clipboard)
{
    touchPosition touch = {0};
    if (pressed & KEY_TOUCH) {
        hidTouchRead(&touch);
        draft->keypad_focus = false;
    }
    if (draft->edit_caret) {
        if (repeat & KEY_LEFT) {
            draft->caret = text_previous(draft->text, draft->text + draft->caret) - draft->text;
        } else if (repeat & KEY_RIGHT) {
            draft->caret = text_next(draft->text + draft->caret) - draft->text;
        } else if (pressed & KEY_UP) {
            draft->caret = 0;
        } else if (pressed & KEY_DOWN) {
            draft->caret = draft->length;
        }
        if ((pressed & KEY_A) || ui_hit(toolbar_left, touch)) {
            paste(draft, clipboard);
        } else if ((pressed & (KEY_B | KEY_R)) || ui_hit(toolbar_right, touch)) {
            draft->edit_caret = false;
        }
        return COMPOSER_CONTINUE;
    }
    if (pressed & KEY_B) {
        if (!draft->symbols) return COMPOSER_BACK;
        toggle_symbols(draft);
        return COMPOSER_CONTINUE;
    }

    bool pick_words = *draft->digits && !draft->symbols;
    if ((pressed & KEY_SELECT) || ui_hit(language_button, touch)) {
        change_language(draft);
        return COMPOSER_CONTINUE;
    }
    if ((pressed & KEY_ZR) || ui_hit(symbols_button, touch)) {
        toggle_symbols(draft);
        return COMPOSER_CONTINUE;
    }
    if ((pressed & KEY_START) || ui_hit(send_button, touch)) {
        return !*draft->digits && has_text(draft) ? COMPOSER_SEND : COMPOSER_CONTINUE;
    }
    if (pressed || repeat) draft->notice = NULL;
    if (pick_words) {
        if (((pressed & KEY_L) || ui_hit(previous_words, touch)) && draft->candidate_page) {
            draft->candidate_page--;
        } else if (((pressed & KEY_R) || ui_hit(next_words, touch)) && pinyin_candidates(draft->digits, draft->candidate_page).more) {
            draft->candidate_page++;
        }
        for (unsigned i = 0; i < PINYIN_CANDIDATES; i++) {
            if (ui_hit(candidate_rect(i), touch)) {
                commit_candidate(draft, i, false);
                return COMPOSER_CONTINUE;
            }
        }
    } else if (draft->symbols) {
        for (unsigned i = 0; i < sizeof(symbol_tabs) / sizeof(symbol_tabs[0]); i++) {
            if (ui_hit(symbol_tab_rects[i], touch)) {
                draft->symbol_page = i;
                return COMPOSER_CONTINUE;
            }
        }
    } else {
        if ((pressed & KEY_L) || ui_hit(toolbar_right, touch)) return COMPOSER_VOICE;
        if ((pressed & KEY_R) || ui_hit(toolbar_middle, touch)) {
            draft->edit_caret = true;
            return COMPOSER_CONTINUE;
        }
        if (ui_hit(toolbar_left, touch)) {
            paste(draft, clipboard);
            return COMPOSER_CONTINUE;
        }
    }

    bool quick_english = draft->latin && !draft->symbols;
    if (quick_english && ui_hit(english_comma, touch)) composer_insert_text(draft, ",");
    if (quick_english && ui_hit(english_period, touch)) composer_insert_text(draft, ".");
    if (repeat & KEY_Y) erase(draft);
    if ((pressed & KEY_X) || ui_hit(quick_english ? english_space : space_button, touch)) {
        space_or_commit(draft);
    }
    unsigned rows = keyboard_rows(draft);
    if (repeat & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) draft->keypad_focus = true;
    if (repeat & KEY_UP) {
        draft->row = (draft->row + rows - 1) % rows;
    } else if (repeat & KEY_DOWN) {
        draft->row = (draft->row + 1) % rows;
    }
    unsigned columns = keyboard_columns(draft, draft->row);
    if (draft->column >= columns) draft->column = columns - 1;
    if (repeat & KEY_LEFT) {
        draft->column = (draft->column + columns - 1) % columns;
    } else if (repeat & KEY_RIGHT) {
        draft->column = (draft->column + 1) % columns;
    }
    if (pressed & KEY_A) {
        input_key(draft, keyboard_key(draft, draft->row, draft->column));
    }
    if (pressed & KEY_TOUCH) {
        for (unsigned row = 0; row < rows; row++) {
            for (unsigned column = 0; column < keyboard_columns(draft, row); column++) {
                KeyboardKey key = keyboard_key(draft, row, column);
                if (ui_hit(key.rect, touch)) {
                    draft->row = row;
                    draft->column = column;
                    input_key(draft, key);
                    return COMPOSER_CONTINUE;
                }
            }
        }
    }
    return COMPOSER_CONTINUE;
}

static void draw_draft(const Composer *draft, Screen top, const char *title, bool follow_up, bool answering)
{
    ui_clear(top);
    ui_title(top, title, NULL, UI_MUTED);
    ui_card(top, (Rect){16, 42, 368, 164}, UI_PANEL, 0);
    write_text(top, 26, 50, answering ? "填写回答" : follow_up ? "追加给当前轮次" : "消息草稿", UI_ACCENT);

    const char *cursor = draft->text + draft->caret;
    const char *rows[8];
    unsigned count = 0;
    const char *start = draft->text;
    for (;;) {
        rows[count++ % 8] = start;
        const char *end = line_end(start, 344);
        const char *next = *end == '\n' ? end + 1 : end;
        if (!*next || cursor < next) {
            break;
        }
        start = next;
    }
    unsigned first = count > 5 ? count - 5 : 0;
    unsigned shown = 0;
    for (unsigned i = first; i < count; i++) {
        const char *line = rows[i % 8];
        const char *end = line_end(line, 344);
        int y = 70 + 17 * shown++;
        text(top, 26, y, line, end, UI_TEXT);
        if (i + 1 == count && cursor >= line && cursor <= end) {
            fill(top, 26 + text_width(line, cursor), y, 2, 16, UI_ACCENT);
        }
    }
    start = line_end(rows[(count - 1) % 8], 344);
    if (*start == '\n') {
        start++;
    }
    while (*start && shown < 8) {
        const char *end = line_end(start, 344);
        text(top, 26, 70 + 17 * shown++, start, end, UI_TEXT);
        start = *end == '\n' ? end + 1 : end;
    }
    write_text(top, 26, 214, draft->notice ? draft->notice : answering ? "确定后返回问题表单" : "Codex 默认使用简体中文回复", UI_ACCENT);
}

static void centered_label(Screen screen, Rect rect, const char *label, u32 color)
{
    const char *end = line_end(label, rect.w);
    int width = text_width(label, end);
    text(screen, rect.x + (rect.w - width) / 2, rect.y + (rect.h - 16) / 2, label, end, color);
}

static void draw_keyboard(Screen bottom, const Composer *draft, u32 held)
{
    touchPosition touch = {0};
    if (held & KEY_TOUCH) hidTouchRead(&touch);
    for (unsigned row = 0; row < keyboard_rows(draft); row++) {
        for (unsigned column = 0; column < keyboard_columns(draft, row); column++) {
            KeyboardKey key = keyboard_key(draft, row, column);
            bool current = draft->row == row && draft->column == column;
            bool secondary = key.action == KEY_ERASE || key.action == KEY_SHIFT;
            bool down = current && ((held & KEY_A) || ((held & KEY_TOUCH) && ui_hit(key.rect, touch)));
            ui_card(bottom, key.rect, down ? UI_FOCUS : UI_PANEL,
                    current && draft->keypad_focus ? UI_ACCENT : 0);
            centered_label(bottom, key.rect, key.label, secondary ? UI_ACCENT : UI_TEXT);
        }
    }
}

static void draw_candidates(Screen bottom, const Composer *draft, const PinyinPage *page)
{
    ui_card(bottom, previous_words, UI_PANEL, 0);
    centered_label(bottom, previous_words, "‹", draft->candidate_page ? UI_ACCENT : UI_MUTED);
    ui_card(bottom, next_words, UI_PANEL, 0);
    centered_label(bottom, next_words, "›", page->more ? UI_ACCENT : UI_MUTED);
    if (!pinyin_word(page, 0)) {
        write_text(bottom, 39, 41, "暂无候选，检查拼音数字", UI_MUTED);
    }
    for (unsigned i = 0; i < PINYIN_CANDIDATES; i++) {
        const char *word = pinyin_word(page, i);
        if (!word) break;
        Rect rect = candidate_rect(i);
        ui_card(bottom, rect, i == 0 ? UI_FOCUS : UI_PANEL, 0);
        PinyinKind kind = page->items[i].kind;
        const char *badge = kind == PINYIN_COMPLETION ? "补" : kind == PINYIN_CORRECTION ? "纠" : NULL;
        if (badge) {
            ui_label(bottom, rect.x + 1, rect.y + 7, 16, badge, kind == PINYIN_CORRECTION ? UI_WARNING : UI_ACCENT);
            rect.x += 16;
            rect.w -= 16;
        }
        if (text_width(word, line_end(word, 80)) > rect.w) {
            ui_label(bottom, rect.x, rect.y + 7, rect.w - 16, word, UI_TEXT);
            ui_label(bottom, rect.x + rect.w - 16, rect.y + 7, 16, "…", UI_TEXT);
        } else {
            centered_label(bottom, rect, word, UI_TEXT);
        }
    }
}

void composer_draw(const Composer *draft, Screen top, Screen bottom, const char *title,
                   bool follow_up, bool can_paste, bool answering, u32 held)
{
    draw_draft(draft, top, title, follow_up, answering);
    PinyinPage page = pinyin_candidates(draft->digits, draft->candidate_page);
    if (*page.combined && !draft->candidate_page && !draft->symbols && !draft->notice) {
        ui_card(top, (Rect){16, 207, 368, 28}, UI_PANEL, 0);
        ui_label(top, 25, 213, 80, "组词候选", UI_ACCENT);
        ui_label(top, 106, 213, 266, page.combined, UI_TEXT);
    }
    ui_clear(bottom);
    ButtonTone paste_tone = can_paste ? UI_SECONDARY : UI_DISABLED;
    if (draft->edit_caret) {
        write_text(bottom, 8, 9, "编辑光标", UI_ACCENT);
        ui_button(bottom, toolbar_left, "粘贴", paste_tone);
        ui_button(bottom, toolbar_right, "返回键盘", UI_SECONDARY);
        write_text(bottom, 12, 90, "方向键左右移动光标", UI_TEXT);
        write_text(bottom, 12, 118, "向上移到开头，向下移到末尾", UI_TEXT);
        return;
    }

    if (draft->symbols) {
        write_text(bottom, 8, 9, "数字与符号", UI_ACCENT);
        for (unsigned i = 0; i < sizeof(symbol_tabs) / sizeof(symbol_tabs[0]); i++) {
            ui_button(bottom, symbol_tab_rects[i], symbol_tabs[i],
                      draft->symbol_page == i ? UI_PRIMARY : UI_SECONDARY);
        }
    } else if (*draft->digits) {
        char spelling[64];
        size_t length = strlen(draft->digits);
        const char *visible_digits = draft->digits + (length > 31 ? length - 31 : 0);
        bool prefix_only = page.items[0].kind == PINYIN_PREFIX;
        snprintf(spelling, sizeof(spelling), "%s  %s%s", prefix_only ? "前段" : "拼音",
                 length > 31 ? "…" : "", visible_digits);
        write_text(bottom, 8, 9, spelling, UI_ACCENT);
        draw_candidates(bottom, draft, &page);
    } else {
        const char *mode = draft->latin ? (draft->shift ? "英文 · 大写" : "英文 · 小写") : "中文 · 九宫格拼音";
        write_text(bottom, 8, 9, mode, UI_ACCENT);
        ui_button(bottom, toolbar_left, "粘贴", paste_tone);
        ui_button(bottom, toolbar_middle, "光标", UI_SECONDARY);
        ui_button(bottom, toolbar_right, "语音听写", UI_SECONDARY);
    }
    draw_keyboard(bottom, draft, held);
    ui_button(bottom, symbols_button, draft->symbols ? "返回" : "符号", UI_SECONDARY);
    ui_button(bottom, language_button, draft->latin ? "英" : "中", UI_SECONDARY);
    if (draft->latin && !draft->symbols) {
        ui_button(bottom, english_comma, ",", UI_SECONDARY);
        ui_card(bottom, english_space, UI_PANEL, 0);
        centered_label(bottom, english_space, "空格", UI_TEXT);
        ui_button(bottom, english_period, ".", UI_SECONDARY);
    } else {
        ui_button(bottom, space_button, *draft->digits ? "选首词" : "空格", UI_SECONDARY);
    }
    ui_button(bottom, send_button, answering ? "确定" : "发送", has_text(draft) && !*draft->digits ? UI_PRIMARY : UI_DISABLED);
}
