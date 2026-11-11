#include "display.h"
#include "input_form.h"
#include "pictures.h"
#include "network.h"
#include "pairing.h"
#include "selection.h"
#include "voice.h"
#include "ui.h"

#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_BUFFER_SIZE 0x100000
#define LIST_ROWS 9
#define HISTORY_ROWS 10
#define POLL_INTERVAL_MS 1500
#define HISTORY_WIDTH 344
#define TOUCH_HISTORY_WIDTH 264
#define LIST_TOP 52
#define LIST_STEP 44
#define LIST_CARD_HEIGHT 39
#define VISIBLE_CARDS 4

static const Rect first_left = {16, 48, 140, 37};
static const Rect first_right = {164, 48, 140, 37};
static const Rect second_left = {16, 100, 140, 37};
static const Rect second_right = {164, 100, 140, 37};
static const Rect third_left = {16, 152, 140, 37};
static const Rect third_right = {164, 152, 140, 37};
static const Rect open_button = {16, 100, 288, 37};
static const Rect approval_banner = {177, 7, 127, 29};
static const Rect picture_button = {172, 193, 132, 27};

typedef enum {
    PROJECTS,
    CONVERSATIONS,
    HISTORY,
    APPROVAL,
} ViewKind;

typedef enum {
    READ_VIEW,
    SEND_MESSAGE,
    STEER_TURN,
    STOP_TURN,
    DECIDE_APPROVAL,
    READ_INPUT,
    READ_PICTURE,
    ANSWER_INPUT,
    TRANSCRIBE_VOICE,
    PAIR_DEVICE,
} RequestKind;

typedef struct {
    ViewKind kind;
    char id[97];
    unsigned page;
    unsigned selected;
} View;

typedef struct {
    char *id;
    char *title;
    const char *detail;
} Row;

static BridgeRequest request;
static Pictures pictures;
static char content[RESPONSE_CAPACITY];
static char *heading;
static Row rows[LIST_ROWS];
static unsigned row_count;
static const char *history_lines[RESPONSE_CAPACITY];
static const char *history_start;
static unsigned history_count;
static MessageRange message_ranges[256];
static unsigned message_count;
static char clipboard[MESSAGE_CAPACITY];
static bool has_next;
static char turn_state[24];
static char bridge_session[33];
static char active_turn[97];
static char approval_token[33];
static const char *approval_kind;
static bool can_accept;
static const char *rejection;
static const char *decision;
static const char *action_result;
static bool confirm_stop;
static bool suspended;

static bool awaiting_reply(void)
{
    return !strcmp(turn_state, "running") || !strcmp(turn_state, "inProgress") ||
           !strcmp(turn_state, "approval") || !strcmp(turn_state, "input") || !strcmp(turn_state, "stopping");
}

static bool can_control_turn(void)
{
    return *active_turn && awaiting_reply() && strcmp(turn_state, "stopping") != 0;
}

static const char *status_label(void)
{
    if (!strcmp(turn_state, "approval")) return "等待审批";
    if (!strcmp(turn_state, "input")) return "等待回答";
    if (!strcmp(turn_state, "stopping")) return "正在停止本轮…";
    if (awaiting_reply()) return "Codex 正在执行…";
    if (!strcmp(turn_state, "completed")) return "回复完成";
    if (!strcmp(turn_state, "failed")) return "本轮执行失败";
    if (!strcmp(turn_state, "interrupted")) return "本轮已停止";
    if (!strcmp(turn_state, "unknown")) return "发送状态未知";
    return "";
}

static void show_text(Screen screen, int x, int y, const char *value, u32 color)
{
    ui_label(screen, x, y, screen.width - x - 12, value, color);
}

static char *take_line(char **rest)
{
    char *line = *rest;
    char *newline = strchr(line, '\n');
    if (newline) {
        *newline = '\0';
        *rest = newline + 1;
    } else {
        *rest += strlen(line);
    }
    return line;
}

static void wrap_history(const char *body, int width)
{
    history_count = 0;
    history_start = body;
    const char *cursor = body;
    while (*cursor && history_count < RESPONSE_CAPACITY) {
        history_lines[history_count++] = cursor;
        cursor = line_end(cursor, width);
        if (*cursor == '\n') {
            cursor++;
        }
    }
}

static void load_message_ranges(const char *ranges, size_t length)
{
    message_count = 0;
    while (*ranges && message_count < sizeof(message_ranges) / sizeof(message_ranges[0])) {
        MessageRange range;
        int consumed = 0;
        if (sscanf(ranges, "%u,%u,%u%n", &range.label, &range.body, &range.end, &consumed) != 3 ||
            range.label > range.body || range.body > range.end || range.end > length) {
            break;
        }
        message_ranges[message_count++] = range;
        ranges += consumed;
        if (*ranges != ';') {
            break;
        }
        ranges++;
    }
}

static SelectionPage selection_page(void)
{
    return (SelectionPage){history_start, history_lines, history_count, message_ranges, message_count};
}

static bool load_view(View *view, int history_width)
{
    const char *marker = view->kind == APPROVAL ? "3ds-codex/approval/2\n" : "3ds-codex/5\n";
    size_t marker_length = strlen(marker);
    if (request.response_length >= sizeof(content) || strncmp(request.response, marker, marker_length) != 0) {
        return false;
    }

    strcpy(content, request.response + marker_length);
    char *body = content;
    const char *ranges = "";
    if (view->kind == APPROVAL) {
        snprintf(approval_token, sizeof(approval_token), "%s", take_line(&body));
        const char *choices = take_line(&body);
        can_accept = choices[0] == '1';
        rejection = strlen(choices) == 2 ? choices[1] == '1' ? "decline" :
                    choices[1] == '2' ? "cancel" : NULL : NULL;
        approval_kind = take_line(&body);
        heading = take_line(&body);
        has_next = false;
    } else {
        has_next = strcmp(take_line(&body), "1") == 0;
        heading = take_line(&body);
        snprintf(turn_state, sizeof(turn_state), "%s", take_line(&body));
        snprintf(bridge_session, sizeof(bridge_session), "%s", take_line(&body));
        snprintf(active_turn, sizeof(active_turn), "%s", take_line(&body));
        ranges = take_line(&body);
    }

    pictures.count = 0;
    if (view->kind >= HISTORY) {
        if (view->kind == HISTORY) pictures_load_thumbs(&pictures, ranges, body);
        wrap_history(body, history_width);
        load_message_ranges(ranges, strlen(body));
        unsigned last = history_count > HISTORY_ROWS ? history_count - HISTORY_ROWS : 0;
        if (view->selected > last) {
            view->selected = last;
        }
    } else {
        row_count = 0;
        while (*body && row_count < LIST_ROWS) {
            char *entry = take_line(&body);
            char *separator = strchr(entry, '\t');
            if (separator && separator - entry < (int)sizeof(view->id)) {
                *separator = '\0';
                char *title = separator + 1;
                const char *detail = "";
                if (view->kind == PROJECTS) {
                    char *count = NULL;
                    for (char *marker = strstr(title, "（"); marker; marker = strstr(marker + 3, "（")) {
                        count = marker;
                    }
                    char *closing = count ? strstr(count + 3, " 条）") : NULL;
                    if (closing && closing[sizeof(" 条）") - 1] == '\0') {
                        *count = '\0';
                        closing[sizeof(" 条") - 1] = '\0';
                        detail = count + 3;
                    }
                }
                rows[row_count++] = (Row){entry, title, detail};
            }
        }
        if (view->selected >= row_count) {
            view->selected = row_count ? row_count - 1 : 0;
        }
    }
    return true;
}

static const char *approval_label(void)
{
    if (!strcmp(approval_kind, "file")) return "文件变更审批";
    if (!strcmp(approval_kind, "command")) return "命令执行审批";
    return "审批请求";
}

static int message_for(const char *line)
{
    unsigned offset = line - history_start;
    for (unsigned i = 0; i < message_count; i++) {
        if (offset >= message_ranges[i].label && offset < message_ranges[i].end) {
            return (int)i;
        }
    }
    return -1;
}

static unsigned scroll_limit(ViewKind kind)
{
    if (kind < HISTORY) {
        return row_count ? row_count - 1 : 0;
    }
    return history_count > HISTORY_ROWS ? history_count - HISTORY_ROWS : 0;
}

static void reflow_history(View *view, int width, bool follow)
{
    const char *anchor = history_count ? history_lines[view->selected] : history_start;
    wrap_history(history_start, width);
    unsigned row = follow ? scroll_limit(view->kind) : 0;
    while (!follow && row < scroll_limit(view->kind) && history_lines[row + 1] <= anchor) {
        row++;
    }
    view->selected = row;
}

static int picture_for_row(unsigned row)
{
    return row < history_count ? pictures_at(&pictures, history_lines[row] - history_start) : -1;
}

static int picture_for_view(View view)
{
    for (unsigned row = view.selected; row < view.selected + HISTORY_ROWS && row < history_count; row++) {
        int picture = picture_for_row(row);
        if (picture >= 0) return picture;
    }
    return pictures.count ? 0 : -1;
}

static void prepare_picture(View view, unsigned selected)
{
    pictures_prepare(&pictures, selected);
    snprintf(request.path, sizeof(request.path), "/threads/%s/images/%s",
             view.id, pictures.thumbs[selected].token);
    request.message[0] = '\0';
}

static void draw_history(Screen screen, View view)
{
    unsigned last = view.selected + HISTORY_ROWS;
    if (last > history_count) last = history_count;
    unsigned row = view.selected;
    while (row < last) {
        unsigned end = row + 1;
        int message = view.kind == HISTORY ? message_for(history_lines[row]) : -1;
        bool card = message >= 0 || (view.kind == APPROVAL && *history_lines[row] != '\n');
        if (message >= 0) {
            while (end < last && message_for(history_lines[end]) == message) end++;
        } else if (card) {
            while (end < last && *history_lines[end] != '\n') end++;
        }
        int y = 48 + 18 * (row - view.selected);
        if (card) {
            ui_card(screen, (Rect){16, y - 3, screen.width - 32, 18 * (end - row) + 4}, UI_PANEL, 0);
        }
        for (unsigned i = row; i < end; i++) {
            const char *line = history_lines[i];
            int picture = view.kind == HISTORY ? picture_for_row(i) : -1;
            int line_y = y + 18 * (i - row);
            if (picture >= 0) {
                const PictureThumb *thumb = &pictures.thumbs[picture];
                unsigned slice = ((unsigned)(line - history_start) - thumb->offset) / 2;
                fill(screen, 16, line_y, 144, 18, UI_PANEL);
                if (thumb->width && slice < 2) {
                    pictures_draw_thumb(thumb, screen, 28, line_y, slice);
                } else {
                    ui_label(screen, 28, line_y, 122, "图片附件", UI_ACCENT);
                }
                continue;
            }
            u32 color = UI_TEXT;
            if (message >= 0 && line < history_start + message_ranges[message].body) {
                color = UI_MUTED;
            } else if (view.kind == APPROVAL && (line == history_start || line[-1] == '\n')) {
                if (!strcmp(approval_kind, "file")) {
                    color = *line == '+' ? UI_ACCENT : *line == '-' ? UI_DANGER : UI_TEXT;
                }
                if (!strncmp(line, "命令\n", strlen("命令\n")) ||
                    !strncmp(line, "工作目录\n", strlen("工作目录\n")) ||
                    !strncmp(line, "理由\n", strlen("理由\n"))) {
                    color = UI_MUTED;
                }
            }
            ui_label(screen, 28, line_y, screen.width - 56, line, color);
        }
        row = end;
    }
}

static unsigned first_card(View view)
{
    return view.selected >= VISIBLE_CARDS ? view.selected - VISIBLE_CARDS + 1 : 0;
}

static void draw_list(Screen screen, View view)
{
    if (!row_count) {
        ui_card(screen, (Rect){16, 54, screen.width - 32, 44}, UI_PANEL, 0);
        show_text(screen, 28, 68, "这里还没有对话。", UI_MUTED);
        return;
    }
    unsigned first = first_card(view);
    for (unsigned i = first; i < row_count && i < first + VISIBLE_CARDS; i++) {
        int y = LIST_TOP + (i - first) * LIST_STEP;
        ui_card(screen, (Rect){16, y, screen.width - 32, LIST_CARD_HEIGHT}, UI_PANEL, i == view.selected ? UI_ACCENT : 0);
        char count[32] = "";
        if (*rows[i].detail) {
            snprintf(count, sizeof(count), "%s对话", rows[i].detail);
        }
        int detail_width = text_width(count, line_end(count, 112));
        ui_label(screen, 28, y + 11, screen.width - 56 - (detail_width ? detail_width + 12 : 0), rows[i].title, UI_TEXT);
        if (detail_width) {
            ui_label(screen, screen.width - 30 - detail_width, y + 11, detail_width, count, UI_MUTED);
        }
    }
}

static void draw_confirmation(Screen bottom, const char *prompt)
{
    ui_card(bottom, (Rect){16, 51, 288, 67}, UI_PANEL, 0);
    show_text(bottom, 29, 76, prompt, UI_WARNING);
    ui_button(bottom, third_left, "B 取消", UI_SECONDARY);
    ui_button(bottom, third_right, "A 确认", UI_CAUTION);
}

static bool can_compose(bool retry_action)
{
    return !retry_action && strcmp(turn_state, "input") && (can_control_turn() || (!awaiting_reply() && strcmp(turn_state, "unknown")));
}

static void render(Screen top, Screen bottom, View view, bool loaded, bool loading,
                   bool retry_action, const char *error, const BridgeEndpoint *endpoint, const char *notice,
                   bool swapped)
{
    ui_clear(top);
    ui_clear(bottom);
    if (swapped) {
        Screen controls = top;
        controls.pixels += 40 * controls.height * 3;
        controls.width = bottom.width;
        top = bottom;
        bottom = controls;
    }
    bool pending = !strcmp(turn_state, "approval");
    bool needs_input = view.kind == HISTORY && !strcmp(turn_state, "input");
    char page[64];
    if (view.kind == CONVERSATIONS && !view.page && !has_next) {
        snprintf(page, sizeof(page), "%u 条对话", row_count);
    } else {
        snprintf(page, sizeof(page), "第 %u 页%s", view.page + 1, has_next ? " +" : "");
    }
    const char *title = !loaded ? "3DS Codex" : view.kind == PROJECTS ? "你的项目" :
                        view.kind == APPROVAL ? approval_label() : heading;
    const char *detail = loaded ? view.kind == APPROVAL || (view.kind == HISTORY && pending) ? "待审批" :
                         needs_input ? "待回答" : view.kind == PROJECTS && endpoint->key_length ? "局域网" : page : NULL;
    ui_title(top, title, detail, view.kind == APPROVAL || pending || needs_input ? UI_WARNING : UI_ACCENT);
    ui_connection(bottom, *error ? "连接或请求失败" : loading ? "正在连接…" : loaded ? "已连接" : "正在准备…",
                  *error ? UI_WARNING : loaded ? UI_ACCENT : UI_MUTED);
    if (!loaded) {
        ui_card(top, (Rect){16, 54, top.width - 32, 52}, UI_PANEL, 0);
        show_text(top, 28, 73, loading ? "正在读取 Codex 项目…" : "无法连接 Codex", UI_TEXT);
    } else if (view.kind >= HISTORY) {
        draw_history(top, view);
    } else {
        draw_list(top, view);
    }

    if (loaded && view.kind == APPROVAL) {
        if (decision) {
            const char *prompt = !strcmp(decision, "accept") ?
                (!strcmp(approval_kind, "file") ? "批准这次文件变更？" : "批准这次命令执行？") :
                !strcmp(decision, "cancel") ? "拒绝并停止本轮？" : "拒绝这条请求？";
            draw_confirmation(bottom, prompt);
        } else {
            show_text(bottom, 16, 51, heading, UI_TEXT);
            show_text(bottom, 16, 77, "请核对完整的审批详情", UI_MUTED);
            if (rejection) ui_button(bottom, third_left,
                !strcmp(rejection, "cancel") ? "Y 拒绝并停止" : "Y 拒绝", UI_SECONDARY);
            if (can_accept) ui_button(bottom, third_right, "X 批准一次", UI_CAUTION);
        }
    } else if (loaded && view.kind == HISTORY) {
        if (confirm_stop) {
            draw_confirmation(bottom, "停止本轮执行？");
        } else {
            if (pending) ui_button(bottom, approval_banner, "Y 查看审批", UI_CAUTION);
            if (needs_input) ui_button(bottom, approval_banner, "Y 查看问题", UI_CAUTION);
            ui_button(bottom, first_left, "较早消息", has_next ? UI_SECONDARY : UI_DISABLED);
            ui_button(bottom, first_right, "跳到最新", UI_SECONDARY);
            ui_button(bottom, second_left, "A 选择文字", history_count ? UI_SECONDARY : UI_DISABLED);
            ui_button(bottom, second_right, "复制整条", message_count ? UI_SECONDARY : UI_DISABLED);
            if (can_compose(retry_action)) {
                bool active = can_control_turn();
                ui_button(bottom, third_left, active ? "X 追加指令" : "X 键盘", UI_PRIMARY);
                ui_button(bottom, third_right, active ? "停止本轮" : "语音听写", UI_SECONDARY);
            } else if (needs_input && can_control_turn()) {
                ui_button(bottom, third_right, "停止本轮", UI_SECONDARY);
            }
            const char *status = action_result && !loading && !*error ? action_result : status_label();
            ui_label(bottom, 16, 200, pictures.count ? 148 : 288, status, pending ? UI_WARNING : UI_ACCENT);
            if (pictures.count) ui_button(bottom, picture_button, "查看图片", UI_SECONDARY);
        }
    } else if (loaded) {
        ui_button(bottom, first_left, "上一页", view.page ? UI_SECONDARY : UI_DISABLED);
        ui_button(bottom, first_right, "下一页", has_next ? UI_SECONDARY : UI_DISABLED);
        ui_button(bottom, open_button, view.kind == PROJECTS ? "A 打开项目" : "A 打开对话",
                  row_count ? UI_PRIMARY : UI_DISABLED);
        if (view.kind == PROJECTS) {
            ui_button(bottom, third_left, "连接设置", UI_SECONDARY);
            ui_button(bottom, third_right, "X 刷新", UI_SECONDARY);
            char address[64];
            snprintf(address, sizeof(address), "%s  %s:%u",
                     endpoint->key_length ? "局域网加密" : "本机模拟", endpoint->address, endpoint->port);
            show_text(bottom, 16, 200, *notice ? notice : address, UI_MUTED);
        } else {
            ui_button(bottom, (Rect){16, 152, 288, 37}, "B 返回项目", UI_SECONDARY);
        }
    }
    if (loading) {
        show_text(bottom, 16, 200, "正在加载…", UI_ACCENT);
    } else if (*error) {
        ui_card(bottom, (Rect){12, 145, 296, 71}, UI_PANEL, 0);
        ui_label(bottom, 22, 152, 274, error, UI_WARNING);
        ui_label(bottom, 22, 170, 274, line_end(error, 274), UI_WARNING);
        show_text(bottom, 22, 192, retry_action ? "A 重试同一操作" : "A 刷新   B 返回", UI_TEXT);
    }
    if (loaded && !*error && !decision && !confirm_stop) {
        show_text(bottom, 16, 221, swapped ? "滑动下屏  ·  松开 ZL 还原" :
                  view.kind == PROJECTS ? "↑↓ 选择  ·  按住 ZL 换屏" :
                  view.kind == CONVERSATIONS ? "B 返回项目  ·  按住 ZL 换屏" :
                  "B 返回  ·  按住 ZL 滑动", UI_MUTED);
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

static u32 touch_action(View view, touchPosition touch, bool retry_action, bool *copy_message)
{
    if (view.kind == APPROVAL || confirm_stop) {
        if (ui_hit(third_left, touch)) return (decision || confirm_stop) ? KEY_B : rejection ? KEY_Y : 0;
        if (ui_hit(third_right, touch)) return (decision || confirm_stop) ? KEY_A : can_accept ? KEY_X : 0;
        return 0;
    }
    if (view.kind == HISTORY) {
        if ((!strcmp(turn_state, "approval") || !strcmp(turn_state, "input")) &&
            ui_hit(approval_banner, touch)) return KEY_Y;
        if (!strcmp(turn_state, "input") && can_control_turn() && ui_hit(third_right, touch)) return KEY_SELECT;
        if (ui_hit(first_left, touch)) return has_next ? KEY_R : 0;
        if (ui_hit(first_right, touch)) return KEY_ZR;
        if (ui_hit(second_left, touch)) return history_count ? KEY_A : 0;
        if (ui_hit(second_right, touch)) {
            *copy_message = message_count != 0;
            return 0;
        }
        if (can_compose(retry_action)) {
            if (ui_hit(third_left, touch)) return KEY_X;
            if (ui_hit(third_right, touch)) return can_control_turn() ? KEY_SELECT : KEY_X | KEY_L;
        }
        return 0;
    }
    if (ui_hit(first_left, touch)) return KEY_L;
    if (ui_hit(first_right, touch)) return KEY_R;
    if (ui_hit(open_button, touch)) return row_count ? KEY_A : 0;
    if (ui_hit(third_left, touch)) return KEY_B;
    if (ui_hit(third_right, touch)) return view.kind == PROJECTS ? KEY_X : KEY_B;
    return 0;
}

typedef struct {
    bool active;
    bool moved;
    int start_x;
    int start_y;
    int last_y;
} ScrollGesture;

static u32 scroll_touch(ScrollGesture *gesture, View *view, u32 held, u32 pressed,
                        bool *follow, bool *redraw, int *open_picture)
{
    if (pressed & KEY_TOUCH) {
        touchPosition touch;
        hidTouchRead(&touch);
        *gesture = (ScrollGesture){.active = touch.py >= LIST_TOP,
                                   .start_x = touch.px, .start_y = touch.py,
                                   .last_y = touch.py};
        return 0;
    }
    if (!gesture->active) return 0;
    if (!(held & KEY_TOUCH)) {
        gesture->active = false;
        if (gesture->moved || gesture->start_x < 16 || gesture->start_x >= 304) return 0;
        if (view->kind >= HISTORY) {
            if (view->kind == HISTORY && gesture->start_x < 160) {
                *open_picture = picture_for_row(view->selected + (gesture->start_y - 48) / 18);
            }
            return 0;
        }
        int row = gesture->start_y - LIST_TOP;
        unsigned index = first_card(*view) + row / LIST_STEP;
        if (row / LIST_STEP < VISIBLE_CARDS && row % LIST_STEP < LIST_CARD_HEIGHT &&
            index < row_count) {
            view->selected = index;
            *redraw = true;
            return KEY_A;
        }
        return 0;
    }

    touchPosition touch;
    hidTouchRead(&touch);
    int delta = gesture->last_y - touch.py;
    if (abs(gesture->start_y - touch.py) >= 8 || abs(gesture->start_x - touch.px) >= 8) {
        gesture->moved = true;
    }
    int step = view->kind < HISTORY ? LIST_STEP / 2 : 18;
    if (abs(delta) < step) return 0;
    unsigned limit = scroll_limit(view->kind);
    int target = (int)view->selected + delta / step;
    gesture->last_y -= delta / step * step;
    if (target >= 0 && (unsigned)target <= limit) {
        view->selected = target;
        *follow = false;
        *redraw = true;
        return 0;
    }
    unsigned edge = target < 0 ? 0 : limit;
    if (view->selected != edge) {
        view->selected = edge;
        *follow = false;
        *redraw = true;
        return 0;
    }
    if (view->kind == APPROVAL) return 0;
    bool older_or_next = view->kind == HISTORY ? delta < 0 : delta > 0;
    if (older_or_next ? !has_next : !view->page) return 0;
    gesture->active = false;
    return older_or_next ? KEY_R : KEY_L;
}

static void make_read_path(View view, bool refresh)
{
    request.message[0] = '\0';
    switch (view.kind) {
        case PROJECTS:
            snprintf(request.path, sizeof(request.path), "/projects?page=%u%s", view.page,
                     refresh ? "&refresh=1" : "");
            break;
        case CONVERSATIONS:
            snprintf(request.path, sizeof(request.path), "/projects/%s/threads?page=%u", view.id, view.page);
            break;
        case HISTORY:
            snprintf(request.path, sizeof(request.path), "/threads/%s?page=%u", view.id, view.page);
            break;
        case APPROVAL:
            snprintf(request.path, sizeof(request.path), "/threads/%s/approval", view.id);
            break;
    }
}

static void set_operation(const char *message, unsigned *sequence)
{
    snprintf(request.session, sizeof(request.session), "%s", bridge_session);
    snprintf(request.operation, sizeof(request.operation), "%llx-%x",
             (unsigned long long)svcGetSystemTick(), ++*sequence);
    snprintf(request.message, sizeof(request.message), "%s", message);
}

static void on_applet_event(APT_HookType event, void *context)
{
    (void)context;
    if (event == APTHOOK_ONSUSPEND || event == APTHOOK_ONSLEEP) {
        suspended = true;
    }
}

int main(void)
{
    gfxInitDefault();
    Screen top = screen_init(GFX_TOP);
    Screen bottom = screen_init(GFX_BOTTOM);
    void *buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    bool network_ready = buffer && R_SUCCEEDED(socInit(buffer, SOC_BUFFER_SIZE));
    char error[128] = "";
    if (!network_ready) {
        snprintf(error, sizeof(error), "无法初始化网络。");
    }

    View views[4] = {{.kind = PROJECTS}};
    View next = views[PROJECTS];
    ViewKind current = PROJECTS;
    Thread worker = NULL;
    bool loaded = false;
    bool refresh = false;
    bool fetch = false;
    bool redraw = true;
    bool swapped = false;
    ScrollGesture gesture = {0};
    Setup setup = {0};
    setup_load(&setup);
    BridgeEndpoint endpoint = {0};
    if (!network_ready) {
        snprintf(setup.notice, sizeof(setup.notice), "%s", error);
    }
    bool composing = false;
    RequestKind job = READ_VIEW;
    RequestKind retry = READ_VIEW;
    char compose_turn[97] = "";
    char stop_turn[97] = "";
    bool follow = false;
    u64 poll_at = 0;
    unsigned sequence = 0;
    static Composer draft;
    static Composer answer_draft;
    static InputForm form;
    bool form_open = false;
    Selection selection = {0};
    static Voice voice;
    u64 last_voice_draw = 0;
    aptHookCookie applet_hook;
    aptHook(&applet_hook, on_applet_event, NULL);
    char draft_thread[97] = "";
    hidSetRepeatParameters(20, 5);

    while (aptMainLoop()) {
        hidScanInput();
        u32 pressed = hidKeysDown();
        u32 repeat = hidKeysDownRepeat();
        u32 held = hidKeysHeld();
        if (!composing && !form_open && setup.phase != SETUP_FORM && setup.phase != SETUP_BUSY &&
            (pressed & KEY_START)) {
            break;
        }

        if (suspended) {
            suspended = false;
            if (voice.phase == VOICE_RECORDING) {
                voice_close(&voice);
                (form_open ? &answer_draft : &draft)->notice = "录音因休眠或切换应用而取消";
                redraw = true;
            } else if (voice.phase == VOICE_TRANSCRIBING) {
                voice.phase = VOICE_CANCELLING;
                redraw = true;
            }
        }
        if (worker && voice.phase == VOICE_TRANSCRIBING && (pressed & (KEY_B | KEY_TOUCH))) {
            touchPosition touch = {0};
            if (pressed & KEY_TOUCH) {
                hidTouchRead(&touch);
            }
            if ((pressed & KEY_B) || (touch.py >= 119 && touch.py < 157)) {
                voice.phase = VOICE_CANCELLING;
                redraw = true;
            }
        }

        if (worker && threadJoin(worker, 0) == 0) {
            threadFree(worker);
            worker = NULL;
            if (job == PAIR_DEVICE) {
                if (setup_receive(&setup, &request, &endpoint)) {
                    next = (View){.kind = PROJECTS};
                    current = PROJECTS;
                    loaded = false;
                    fetch = true;
                }
            } else if (job == TRANSCRIBE_VOICE) {
                voice_receive(&voice, request.response, request.error);
                request.audio = NULL;
                request.audio_length = 0;
            } else if (job == READ_PICTURE) {
                if (*request.error) {
                    snprintf(pictures.error, sizeof(pictures.error), "%s", request.error);
                } else if (!pictures_load(&pictures, request.response, request.response_length)) {
                    snprintf(pictures.error, sizeof(pictures.error), "图片数据无效。");
                }
            } else if (*request.error) {
                snprintf(error, sizeof(error), "%s", request.error);
                retry = job != DECIDE_APPROVAL && !request.rejected ? job : READ_VIEW;
                if (job == DECIDE_APPROVAL) {
                    decision = NULL;
                    can_accept = false;
                    rejection = NULL;
                }
            } else if (job == READ_INPUT) {
                form_open = form_load(&form, request.response);
                if (!form_open) snprintf(error, sizeof(error), "桥接返回了无法识别的问题。");
                else retry = READ_VIEW;
            } else if (job == ANSWER_INPUT) {
                const char marker[] = "3ds-codex/answer/1\nresolved\n";
                if (!strcmp(request.response, marker)) {
                    form_open = false;
                    retry = READ_VIEW;
                    action_result = "答案已提交";
                    next = views[HISTORY];
                    next.page = 0;
                    follow = true;
                    fetch = true;
                } else {
                    snprintf(error, sizeof(error), "提交结果尚未确认。");
                    retry = ANSWER_INPUT;
                }
            } else if (job == DECIDE_APPROVAL) {
                const char marker[] = "3ds-codex/decision/1\nresolved\n";
                action_result = strcmp(request.response, marker) == 0 ?
                    (strcmp(request.message, "accept") == 0 ? "已批准一次" :
                     strcmp(request.message, "cancel") == 0 ? "已拒绝，正在停止本轮" : "已拒绝") :
                    "审批结果尚未确认";
                next = views[HISTORY];
                next.page = 0;
                follow = true;
                decision = NULL;
                fetch = true;
            } else if (job != READ_VIEW) {
                const char *marker = job == SEND_MESSAGE ? "3ds-codex/send/1\n" : "3ds-codex/control/1\n";
                size_t marker_length = strlen(marker);
                if (strncmp(request.response, marker, marker_length) != 0) {
                    snprintf(error, sizeof(error), "操作结果未知。");
                    retry = job;
                } else {
                    const char *result = request.response + marker_length;
                    bool accepted = job == SEND_MESSAGE ? strcmp(result, "unknown\n") != 0 :
                                                          strcmp(result, "accepted\n") == 0;
                    if (accepted && job != STOP_TURN) {
                        draft = (Composer){0};
                    }
                    if (!accepted) {
                        action_result = "操作结果尚未确认";
                    } else if (job == STEER_TURN) {
                        action_result = "已接收追加指令";
                    } else if (job == STOP_TURN) {
                        action_result = "已请求停止";
                    }
                    retry = READ_VIEW;
                    follow = true;
                    next = views[HISTORY];
                    next.page = 0;
                    next.selected = 0;
                    fetch = true;
                }
            } else if (load_view(&next, swapped ? TOUCH_HISTORY_WIDTH : HISTORY_WIDTH)) {
                current = next.kind;
                if (current == HISTORY && next.page == 0 && follow && history_count > HISTORY_ROWS) {
                    next.selected = history_count - HISTORY_ROWS;
                }
                views[current] = next;
                loaded = true;
                poll_at = osGetTime() + POLL_INTERVAL_MS;
            } else {
                snprintf(error, sizeof(error), "桥接返回了无法识别的内容。");
            }
            job = READ_VIEW;
            redraw = true;
        }

        View *view = &views[current];
        Composer *editor = form_open ? &answer_draft : &draft;
        bool browsing = loaded && !pictures.open && !form_open && setup.phase == SETUP_CLOSED && !composing &&
                        !selection.cursor && voice.phase == VOICE_CLOSED &&
                        !confirm_stop && !decision && !*error;
        bool hold_to_swap = browsing && (held & KEY_ZL);
        if (hold_to_swap != swapped) {
            swapped = hold_to_swap;
            gesture.active = false;
            if (!swapped) pressed &= ~KEY_TOUCH;
            if (current >= HISTORY && loaded && !selection.cursor && !*error) {
                unsigned previous_row = view->selected;
                reflow_history(view, swapped ? TOUCH_HISTORY_WIDTH : HISTORY_WIDTH, follow);
                if (worker && job == READ_VIEW && next.kind == current && next.page == view->page &&
                    next.selected == previous_row && !strcmp(next.id, view->id)) {
                    next.selected = view->selected;
                }
            }
            redraw = true;
        }
        if (worker || fetch || !browsing || !swapped) gesture.active = false;
        if (!worker && network_ready && !fetch) {
            touchPosition touch = {0};
            bool copy_message = false;
            int open_picture = -1;
            if (pictures.open && (held & KEY_TOUCH)) {
                hidTouchRead(&touch);
            } else if (browsing && swapped) {
                pressed |= scroll_touch(&gesture, view, held, pressed, &follow, &redraw, &open_picture);
                pressed &= ~KEY_TOUCH;
            } else if (pressed & KEY_TOUCH) {
                hidTouchRead(&touch);
                if (browsing && current == HISTORY && pictures.count && ui_hit(picture_button, touch)) {
                    open_picture = picture_for_view(*view);
                } else if (browsing || (loaded && !*error && (confirm_stop || decision))) {
                    pressed |= touch_action(*view, touch, retry != READ_VIEW, &copy_message);
                }
            }
            if (browsing && current == HISTORY && (pressed & KEY_Y) &&
                strcmp(turn_state, "approval") && strcmp(turn_state, "input")) {
                open_picture = picture_for_view(*view);
            }
            if (pictures.open || open_picture >= 0) {
                if (pictures.open) redraw |= pictures_input(&pictures, pressed, held, touch, &open_picture);
                if (open_picture >= 0) {
                    prepare_picture(*view, open_picture);
                    job = READ_PICTURE;
                    fetch = true;
                }
            } else if (setup.phase != SETUP_CLOSED) {
                SetupAction action = setup_input(&setup, pressed, repeat, &endpoint);
                if (action == SETUP_CONNECT) {
                    setup.phase = SETUP_CLOSED;
                    setup.notice[0] = '\0';
                    next = (View){.kind = PROJECTS};
                    current = PROJECTS;
                    loaded = false;
                    fetch = true;
                } else if (action == SETUP_PAIR) {
                    snprintf(request.path, sizeof(request.path), "/pair");
                    request.message[0] = '\0';
                    job = PAIR_DEVICE;
                    fetch = true;
                }
                redraw |= pressed || repeat;
            } else if (voice.phase != VOICE_CLOSED) {
                bool primary = (pressed & KEY_A) || (touch.py >= 68 && touch.py < 106);
                bool cancel = (pressed & KEY_B) || (touch.py >= 119 && touch.py < 157);
                if (cancel) {
                    voice_close(&voice);
                    redraw = true;
                } else if ((voice.phase == VOICE_READY || voice.phase == VOICE_ERROR) && primary) {
                    voice_start(&voice);
                    redraw = true;
                } else if (voice.phase == VOICE_RECORDING && (primary || voice_recording_finished(&voice))) {
                    if (voice_stop(&voice)) {
                        snprintf(request.path, sizeof(request.path), "/voice/transcribe");
                        snprintf(request.session, sizeof(request.session), "%s", bridge_session);
                        request.audio = voice.samples;
                        request.audio_length = voice.length;
                        job = TRANSCRIBE_VOICE;
                        fetch = true;
                    }
                    redraw = true;
                } else if (voice.phase == VOICE_RESULT && (pressed & KEY_X)) {
                    voice_start(&voice);
                    redraw = true;
                } else if (voice.phase == VOICE_RESULT && primary) {
                    if (composer_insert_text(editor, voice.transcript)) {
                        voice_close(&voice);
                        editor->notice = "已插入转写，可先检查文字";
                    } else {
                        snprintf(voice.error, sizeof(voice.error), "草稿空间不足，未插入转写。");
                    }
                    redraw = true;
                } else if (voice.phase == VOICE_RESULT && (repeat & (KEY_UP | KEY_DOWN))) {
                    voice_scroll(&voice, repeat);
                    redraw = true;
                }
            } else if (selection.cursor) {
                SelectionPage page = selection_page();
                if (!selection_input(&selection, &page, pressed, repeat, touch, clipboard)) {
                    wrap_history(history_start, swapped ? TOUCH_HISTORY_WIDTH : HISTORY_WIDTH);
                    page = selection_page();
                    unsigned row = selection_line(&page, selection.cursor);
                    unsigned max = history_count > HISTORY_ROWS ? history_count - HISTORY_ROWS : 0;
                    view->selected = row < max ? row : max;
                    selection = (Selection){0};
                }
                redraw |= pressed || repeat;
            } else if (confirm_stop && (pressed & KEY_B)) {
                confirm_stop = false;
                redraw = true;
            } else if (confirm_stop && (pressed & KEY_A)) {
                confirm_stop = false;
                snprintf(request.path, sizeof(request.path), "/threads/%s/turns/%s/stop", view->id, stop_turn);
                set_operation("stop", &sequence);
                job = STOP_TURN;
                fetch = true;
            } else if (confirm_stop) {
                // Keep the selected turn pinned while the confirmation is open.
            } else if (current == APPROVAL && decision && (pressed & KEY_B)) {
                decision = NULL;
                redraw = true;
            } else if (current == APPROVAL && decision && (pressed & KEY_A)) {
                snprintf(request.path, sizeof(request.path), "/threads/%s/approval/%s",
                         view->id, approval_token);
                snprintf(request.session, sizeof(request.session), "%s", bridge_session);
                snprintf(request.operation, sizeof(request.operation), "%s", approval_token);
                snprintf(request.message, sizeof(request.message), "%s", decision);
                job = DECIDE_APPROVAL;
                fetch = true;
            } else if (current == APPROVAL && !decision && !*error &&
                       ((can_accept && (pressed & KEY_X)) || (rejection && (pressed & KEY_Y)))) {
                decision = (pressed & KEY_X) && can_accept ? "accept" : rejection;
                redraw = true;
            } else if (!composing && loaded && current == HISTORY && !strcmp(turn_state, "approval") &&
                       (pressed & KEY_Y)) {
                next = *view;
                next.kind = APPROVAL;
                next.selected = 0;
                decision = NULL;
                action_result = NULL;
                fetch = true;
            } else if (!composing && !form_open && !*error && loaded && current == HISTORY &&
                       !strcmp(turn_state, "input") && (pressed & KEY_Y)) {
                snprintf(request.path, sizeof(request.path), "/threads/%s/input", view->id);
                request.message[0] = '\0';
                job = READ_INPUT;
                fetch = true;
            } else if (composing) {
                ComposeAction action = composer_input(editor, pressed, repeat, clipboard);
                if (action == COMPOSER_VOICE) {
                    voice.phase = VOICE_READY;
                } else if (action == COMPOSER_BACK) {
                    composing = false;
                } else if (action == COMPOSER_SEND) {
                    composing = false;
                    if (form_open) {
                        FormQuestion *q = &form.questions[form.page];
                        memcpy(q->text, answer_draft.text, answer_draft.length + 1);
                        q->answer = q->option_count;
                        form.notice = "已填写，可继续查看问题";
                    } else {
                        next = *view;
                        if (*compose_turn) {
                            snprintf(request.path, sizeof(request.path), "/threads/%s/turns/%s/steer",
                                     view->id, compose_turn);
                            job = STEER_TURN;
                        } else {
                            snprintf(request.path, sizeof(request.path), "/threads/%s/send", view->id);
                            job = SEND_MESSAGE;
                        }
                        set_operation(draft.text, &sequence);
                        action_result = NULL;
                        fetch = true;
                    }
                }
                redraw |= pressed || repeat;
            } else if (form_open) {
                if (*error && retry == ANSWER_INPUT &&
                    ((pressed & KEY_A) || ui_hit((Rect){167, 192, 145, 34}, touch))) {
                    job = retry;
                    fetch = true;
                } else if (*error) {
                    if ((pressed & KEY_B) || ui_hit((Rect){8, 192, 145, 34}, touch)) {
                        form_open = false;
                        next = *view;
                        job = retry = READ_VIEW;
                        fetch = true;
                    }
                } else {
                    FormAction action = form_input(&form, pressed, repeat, touch);
                    if (action == FORM_BACK) {
                        form_open = false;
                        next = *view;
                        fetch = true;
                    } else if (action == FORM_EDIT) {
                        answer_draft = (Composer){0};
                        composer_insert_text(&answer_draft, form.questions[form.page].text);
                        composing = true;
                    } else if (action == FORM_SUBMIT) {
                        if (form_write(&form, request.message, sizeof(request.message))) {
                            snprintf(request.path, sizeof(request.path), "/threads/%s/input/%s", view->id, form.token);
                            snprintf(request.session, sizeof(request.session), "%s", bridge_session);
                            snprintf(request.operation, sizeof(request.operation), "%s", form.token);
                            job = ANSWER_INPUT;
                            fetch = true;
                        } else {
                            form.notice = "回答太长，请缩短后再提交";
                        }
                    }
                }
                redraw |= pressed || repeat;
            } else if (*error && (pressed & KEY_A)) {
                job = retry;
                if (retry == READ_VIEW && loaded) {
                    next = *view;
                }
                fetch = true;
            } else if (loaded && current == HISTORY && history_count && !*error &&
                       (pressed & KEY_A)) {
                const char *start = history_lines[view->selected];
                wrap_history(history_start, 296);
                SelectionPage page = selection_page();
                selection = (Selection){.cursor = start, .top = selection_line(&page, start),
                                       .notice = "点文字或方向键移动光标"};
                follow = false;
                redraw = true;
            } else if (loaded && current == HISTORY && (pressed & KEY_ZR)) {
                follow = true;
                if (view->page) {
                    next = *view;
                    next.page = 0;
                    fetch = true;
                } else {
                    view->selected = history_count > HISTORY_ROWS ? history_count - HISTORY_ROWS : 0;
                    redraw = true;
                }
            } else if (loaded && current == HISTORY && copy_message && history_count) {
                SelectionPage page = selection_page();
                action_result = selection_copy_message(&page, history_lines[view->selected], clipboard);
                redraw = true;
            } else if (loaded && current == HISTORY && (pressed & KEY_SELECT) &&
                       can_control_turn() && retry == READ_VIEW) {
                memcpy(stop_turn, active_turn, sizeof(stop_turn));
                confirm_stop = true;
                redraw = true;
            } else if (loaded && current == HISTORY && (pressed & KEY_X) && can_compose(retry != READ_VIEW)) {
                memcpy(compose_turn, active_turn, sizeof(compose_turn));
                if (strcmp(draft_thread, view->id)) {
                    draft = (Composer){0};
                    memcpy(draft_thread, view->id, sizeof(draft_thread));
                }
                composing = true;
                if (pressed & KEY_L) {
                    voice.phase = VOICE_READY;
                }
                redraw = true;
            } else if (current == PROJECTS && (pressed & KEY_B)) {
                setup.phase = SETUP_MENU;
                setup.notice[0] = '\0';
                error[0] = '\0';
                retry = READ_VIEW;
                redraw = true;
            } else if (loaded && (pressed & KEY_B) && current > PROJECTS) {
                next = views[current - 1];
                follow = false;
                retry = READ_VIEW;
                decision = NULL;
                if (current != APPROVAL) {
                    action_result = NULL;
                }
                fetch = true;
            } else if (loaded && current == PROJECTS && (pressed & KEY_X)) {
                next = *view;
                refresh = true;
                fetch = true;
            } else if (loaded && current != APPROVAL && ((pressed & KEY_R && has_next) || (pressed & KEY_L && view->page))) {
                next = *view;
                next.page = pressed & KEY_R && has_next ? next.page + 1 : next.page - 1;
                next.selected = 0;
                follow = false;
                retry = READ_VIEW;
                fetch = true;
            } else if (loaded && (pressed & KEY_A) && current < HISTORY && row_count) {
                next = (View){.kind = current + 1};
                snprintf(next.id, sizeof(next.id), "%s", rows[view->selected].id);
                fetch = true;
            } else if (loaded) {
                unsigned limit = scroll_limit(current);
                if (repeat & KEY_DOWN && view->selected < limit) {
                    view->selected++;
                    redraw = true;
                } else if (repeat & KEY_UP && view->selected) {
                    view->selected--;
                    follow = false;
                    redraw = true;
                }
                if (current == HISTORY && view->page == 0 && awaiting_reply() &&
                    !*error && !gesture.active && osGetTime() >= poll_at) {
                    next = *view;
                    fetch = true;
                }
            }
        }

        if (fetch && !worker) {
            fetch = false;
            error[0] = '\0';
            if (job == READ_VIEW) {
                make_read_path(next, refresh);
            }
            refresh = false;
            request.endpoint = endpoint;
            worker = threadCreate(fetch_bridge, &request, 48 * 1024, 0x30, -2, false);
            if (!worker) {
                if (job == PAIR_DEVICE) {
                    snprintf(request.error, sizeof(request.error), "无法发起配对，请重试。");
                    setup_receive(&setup, &request, &endpoint);
                } else if (job == READ_PICTURE) {
                    snprintf(pictures.error, sizeof(pictures.error), "无法发起图片请求。");
                } else if (job == TRANSCRIBE_VOICE) {
                    voice_receive(&voice, "", "无法上传录音，请重试。");
                    request.audio = NULL;
                    request.audio_length = 0;
                } else {
                    snprintf(error, sizeof(error), "无法发起连接。");
                    retry = job == DECIDE_APPROVAL ? READ_VIEW : job;
                }
                job = READ_VIEW;
            }
            redraw = true;
        }
        if (voice.phase == VOICE_RECORDING && osGetTime() - last_voice_draw >= 250) {
            last_voice_draw = osGetTime();
            redraw = true;
        }
        if (redraw) {
            if (setup.phase != SETUP_CLOSED) {
                setup_draw(top, bottom, &setup);
                gfxFlushBuffers();
                gfxSwapBuffers();
            } else if (voice.phase != VOICE_CLOSED) {
                voice_draw(&voice, top, bottom, form_open ? "语音回答" : heading);
                gfxFlushBuffers();
                gfxSwapBuffers();
            } else if (selection.cursor) {
                SelectionPage page = selection_page();
                selection_draw(top, bottom, heading, &selection, &page);
                gfxFlushBuffers();
                gfxSwapBuffers();
            } else if (composing) {
                composer_draw(editor, top, bottom, form_open ? "回答 Codex" : heading,
                              *compose_turn != 0, *clipboard != 0, form_open);
                gfxFlushBuffers();
                gfxSwapBuffers();
            } else if (pictures.open) {
                pictures_draw(&pictures, top, bottom);
                gfxFlushBuffers();
                gfxSwapBuffers();
            } else if (form_open) {
                form_draw(&form, top, bottom, worker != NULL, error, retry == ANSWER_INPUT);
                gfxFlushBuffers();
                gfxSwapBuffers();
            } else {
                render(top, bottom, views[current], loaded, worker != NULL, retry != READ_VIEW, error, &endpoint, setup.notice,
                       swapped && !confirm_stop && !decision && !*error);
            }
            redraw = false;
        }
        gspWaitForVBlank();
    }

    if (worker) {
        threadJoin(worker, U64_MAX);
        threadFree(worker);
    }
    aptUnhook(&applet_hook);
    voice_close(&voice);
    if (network_ready) {
        socExit();
    }
    free(buffer);
    gfxExit();
    return 0;
}
