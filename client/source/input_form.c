#include "input_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Rect previous = {8, 34, 145, 29};
static const Rect next = {167, 34, 145, 29};
static const Rect back = {8, 192, 145, 34};
static const Rect submit = {167, 192, 145, 34};
static const Rect options_up = {276, 70, 36, 34};
static const Rect options_down = {276, 146, 36, 34};

static char *field(char **cursor)
{
    char *line = *cursor;
    char *end = strchr(line, '\n');
    if (!end) return NULL;
    *end = '\0';
    *cursor = end + 1;
    return line;
}

static int number(char **cursor, unsigned max)
{
    const char *value = field(cursor);
    if (!value || !*value || strspn(value, "0123456789") != strlen(value)) return -1;
    unsigned long parsed = strtoul(value, NULL, 10);
    return parsed <= max ? (int)parsed : -1;
}

bool form_load(InputForm *form, const char *response)
{
    const char marker[] = "3ds-codex/input/1\n";
    if (strncmp(response, marker, sizeof(marker) - 1)) return false;
    const char *body = response + sizeof(marker) - 1;
    if (*form->token && !strncmp(body, form->token, 32) && body[32] == '\n') return true;
    memset(form, 0, sizeof(*form));
    snprintf(form->payload, sizeof(form->payload), "%s", body);
    char *cursor = form->payload;
    const char *token = field(&cursor);
    if (!token || strlen(token) != 32 || strspn(token, "0123456789abcdef") != 32) return false;
    int count = number(&cursor, FORM_QUESTIONS);
    if (count <= 0) return false;
    form->count = count;
    for (unsigned i = 0; i < form->count; i++) {
        FormQuestion *q = &form->questions[i];
        q->header = field(&cursor);
        q->prompt = field(&cursor);
        int flags = number(&cursor, 1);
        int options = number(&cursor, FORM_OPTIONS);
        if (!q->header || !q->prompt || flags < 0 || options < 0 || (!flags && !options)) return false;
        q->free_text = flags;
        q->option_count = options;
        q->answer = -1;
        for (unsigned j = 0; j < q->option_count; j++) {
            q->options[j].label = field(&cursor);
            q->options[j].description = field(&cursor);
            if (!q->options[j].label || !q->options[j].description) return false;
        }
    }
    if (*cursor) return false;
    memcpy(form->token, token, sizeof(form->token));
    return true;
}

static bool complete(const InputForm *form)
{
    for (unsigned i = 0; i < form->count; i++) {
        if (form->questions[i].answer < 0) return false;
    }
    return true;
}

static unsigned first_option(const InputForm *form)
{
    return form->cursor > 2 ? form->cursor - 2 : 0;
}

static Rect option_rect(unsigned index, unsigned count)
{
    return (Rect){8, 70 + index * 38, count > 3 ? 262 : 304, 34};
}

static void show_lines(Screen top, const char *text_value, unsigned *skip, int *y, u32 color)
{
    while (*text_value && *y <= 215) {
        const char *end = line_end(text_value, 344);
        if (*skip) {
            (*skip)--;
        } else {
            text(top, 28, *y, text_value, end, color);
            *y += 18;
        }
        text_value = *end == '\n' ? end + 1 : end;
    }
}

static unsigned line_count(const char *value)
{
    unsigned count = 0;
    while (*value) {
        const char *end = line_end(value, 344);
        value = *end == '\n' ? end + 1 : end;
        count++;
    }
    return count;
}

FormAction form_input(InputForm *form, u32 pressed, u32 repeat, touchPosition touch)
{
    if ((pressed & KEY_B) || ui_hit(back, touch)) return FORM_BACK;
    if ((pressed & KEY_START) || ui_hit(submit, touch)) {
        if (complete(form)) return FORM_SUBMIT;
        form->notice = "请答完所有问题再提交";
        return FORM_CONTINUE;
    }
    if ((pressed & KEY_L || ui_hit(previous, touch)) && form->page > 0) {
        form->page--;
        form->cursor = form->scroll = 0;
    } else if ((pressed & KEY_R || ui_hit(next, touch)) && form->page + 1 < form->count) {
        form->page++;
        form->cursor = form->scroll = 0;
    }
    FormQuestion *q = &form->questions[form->page];
    unsigned count = q->option_count + q->free_text;
    unsigned old_cursor = form->cursor;
    if (((repeat & KEY_DOWN) || (count > 3 && ui_hit(options_down, touch))) && form->cursor + 1 < count) form->cursor++;
    if (((repeat & KEY_UP) || (count > 3 && ui_hit(options_up, touch))) && form->cursor) form->cursor--;
    bool pick = pressed & KEY_A;
    unsigned first = first_option(form);
    for (unsigned i = 0; i < 3 && first + i < count; i++) {
        if (ui_hit(option_rect(i, count), touch)) {
            form->cursor = first + i;
            pick = true;
        }
    }
    if (old_cursor != form->cursor) form->scroll = 0;
    const char *detail = form->cursor < q->option_count ? q->options[form->cursor].description : q->text;
    if ((repeat & KEY_LEFT) && form->scroll) form->scroll--;
    if (repeat & KEY_RIGHT) {
        unsigned lines = line_count(q->prompt) + line_count(detail) + (*detail ? 1 : 0);
        if (lines > form->scroll + 9) form->scroll++;
    }
    if (pick) {
        if (form->cursor == q->option_count) return FORM_EDIT;
        q->answer = form->cursor;
        form->notice = NULL;
    }
    return FORM_CONTINUE;
}

bool form_write(const InputForm *form, char *target, size_t capacity)
{
    if (!complete(form)) return false;
    for (unsigned i = 0; i < form->count; i++) {
        const FormQuestion *q = &form->questions[i];
        int size = (unsigned)q->answer == q->option_count ?
            snprintf(target, capacity, "t%zu\n%s", strlen(q->text), q->text) :
            snprintf(target, capacity, "o%d\n", q->answer);
        if (size < 0 || (size_t)size >= capacity) return false;
        target += size;
        capacity -= size;
    }
    return true;
}

void form_draw(const InputForm *form, Screen top, Screen bottom, bool busy, const char *error, bool retry)
{
    const FormQuestion *q = &form->questions[form->page];
    char page[32];
    snprintf(page, sizeof(page), "%u / %u", form->page + 1, form->count);
    ui_clear(top);
    ui_clear(bottom);
    ui_title(top, *q->header ? q->header : "Codex 提问", page, UI_ACCENT);
    ui_card(top, (Rect){16, 43, 368, 188}, UI_PANEL, 0);
    unsigned skip = form->scroll;
    int y = 51;
    show_lines(top, q->prompt, &skip, &y, UI_TEXT);
    const char *detail = form->cursor < q->option_count ? q->options[form->cursor].description : q->text;
    if (*detail) {
        if (skip) skip--;
        else y += 18;
        show_lines(top, detail, &skip, &y, UI_ACCENT);
    }

    ui_label(bottom, 10, 8, 300, busy ? "正在提交…" : *error ? error :
             form->notice ? form->notice : "等待你的回答", *error ? UI_WARNING : UI_ACCENT);
    if (*error) {
        ui_button(bottom, back, "返回对话", UI_SECONDARY);
        ui_label(bottom, 10, 27, 300, line_end(error, 300), UI_WARNING);
        if (retry) ui_button(bottom, submit, "A 重试提交", UI_PRIMARY);
        return;
    }
    ui_button(bottom, previous, "L 上一题", form->page ? UI_SECONDARY : UI_DISABLED);
    ui_button(bottom, next, "R 下一题", form->page + 1 < form->count ? UI_SECONDARY : UI_DISABLED);
    unsigned first = first_option(form);
    unsigned count = q->option_count + q->free_text;
    for (unsigned i = 0; i < 3 && first + i < count; i++) {
        unsigned index = first + i;
        Rect rect = option_rect(i, count);
        bool selected = q->answer == (int)index;
        ui_card(bottom, rect, selected ? UI_FOCUS : UI_PANEL, index == form->cursor ? UI_ACCENT : 0);
        const char *label = index < q->option_count ? q->options[index].label :
                            selected ? "编辑文字回答" : "填写文字";
        ui_label(bottom, 18, rect.y + 9, rect.w - (selected ? 50 : 20), label, UI_TEXT);
        if (selected) ui_label(bottom, rect.x + rect.w - 41, rect.y + 9, 35, "已选", UI_ACCENT);
    }
    if (count > 3) {
        ui_button(bottom, options_up, "↑", form->cursor ? UI_SECONDARY : UI_DISABLED);
        ui_button(bottom, options_down, "↓", form->cursor + 1 < count ? UI_SECONDARY : UI_DISABLED);
    }
    ui_button(bottom, back, "返回对话", UI_SECONDARY);
    ui_button(bottom, submit, "提交答案", complete(form) ? UI_PRIMARY : UI_DISABLED);
}
