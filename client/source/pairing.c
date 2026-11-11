#include "pairing.h"
#include "ui.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEVICE_FILE "sdmc:/3ds/3ds-codex.pair"
#define DEVICE_TEMP DEVICE_FILE ".tmp"
#define DEVICE_BACKUP DEVICE_FILE ".bak"

enum {
    GRID_TOP = 48,
    GRID_HEIGHT = 30,
};

static const char *keys[] = {"1234567890", "qwertyuiop", "asdfghjkl.", "zxcvbnm---"};

static bool valid_address(const char *value)
{
    struct in_addr address;
    return inet_pton(AF_INET, value, &address) == 1;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

static bool load_credentials(BridgeEndpoint *endpoint, const char *identity, const char *hex)
{
    if (strlen(identity) != 16 || strlen(hex) != 64) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (hex_value(identity[i]) < 0) {
            return false;
        }
    }
    for (unsigned i = 0; i < 32; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        endpoint->key[i] = (unsigned char)(high * 16 + low);
    }
    memcpy(endpoint->identity, identity, 17);
    endpoint->key_length = sizeof(endpoint->key);
    return true;
}

static bool read_saved(const char *path, BridgeEndpoint *endpoint)
{
    FILE *source = fopen(path, "r");
    if (!source) {
        return false;
    }
    char version[20];
    char hex[65];
    char identity[17];
    BridgeEndpoint record = {0};
    int fields = fscanf(source, "%19s %15s %u %16s %64s", version, record.address,
                        &record.port, identity, hex);
    fclose(source);
    if (fields != 5 || strcmp(version, "3DS-CODEX-PAIR-1") || !valid_address(record.address) ||
        !record.port || record.port > 65535 || !load_credentials(&record, identity, hex)) {
        return false;
    }
    *endpoint = record;
    return true;
}

static bool save_endpoint(const BridgeEndpoint *endpoint)
{
    mkdir("sdmc:/3ds", 0777);
    FILE *destination = fopen(DEVICE_TEMP, "w");
    if (!destination) {
        return false;
    }
    bool written = fprintf(destination, "3DS-CODEX-PAIR-1\n%s\n%u\n%s\n",
                           endpoint->address, endpoint->port, endpoint->identity) > 0;
    for (unsigned i = 0; i < sizeof(endpoint->key) && written; i++) {
        written = fprintf(destination, "%02x", endpoint->key[i]) == 2;
    }
    written = fclose(destination) == 0 && written;
    if (!written) {
        remove(DEVICE_TEMP);
        return false;
    }
    if (rename(DEVICE_TEMP, DEVICE_FILE) == 0) {
        remove(DEVICE_BACKUP);
        return true;
    }
    FILE *existing = fopen(DEVICE_FILE, "r");
    if (!existing) {
        remove(DEVICE_TEMP);
        return false;
    }
    fclose(existing);
    remove(DEVICE_BACKUP);
    if (rename(DEVICE_FILE, DEVICE_BACKUP) != 0) {
        remove(DEVICE_TEMP);
        return false;
    }
    if (rename(DEVICE_TEMP, DEVICE_FILE) != 0) {
        rename(DEVICE_BACKUP, DEVICE_FILE);
        remove(DEVICE_TEMP);
        return false;
    }
    remove(DEVICE_BACKUP);
    return true;
}

void setup_load(Setup *setup)
{
    setup->phase = SETUP_MENU;
    strcpy(setup->port, "8766");
    if (read_saved(DEVICE_FILE, &setup->saved) || read_saved(DEVICE_BACKUP, &setup->saved)) {
        memcpy(setup->address, setup->saved.address, sizeof(setup->address));
        snprintf(setup->port, sizeof(setup->port), "%u", setup->saved.port);
    }
}

static void select_key(Setup *setup, char key)
{
    char *target = setup->field == 0 ? setup->address : setup->field == 1 ? setup->port : setup->code;
    size_t limit = setup->field == 0 ? 15 : setup->field == 1 ? 5 : 20;
    size_t length = strlen(target);
    if (length >= limit) {
        return;
    }
    if (setup->field == 2) {
        key = toupper((unsigned char)key);
        if (!((key >= 'A' && key <= 'Z') || (key >= '2' && key <= '7'))) {
            return;
        }
    } else if (!isdigit((unsigned char)key) && !(setup->field == 0 && key == '.')) {
        return;
    }
    target[length] = key;
    target[length + 1] = '\0';
    setup->notice[0] = '\0';
}

SetupAction setup_input(Setup *setup, u32 pressed, u32 repeat, BridgeEndpoint *endpoint)
{
    touchPosition touch = {0};
    if (pressed & KEY_TOUCH) {
        hidTouchRead(&touch);
    }
    if (setup->phase == SETUP_MENU) {
        bool first = (pressed & KEY_A) || (touch.py >= 48 && touch.py < 84);
        bool pair = (pressed & KEY_X) || (touch.py >= 95 && touch.py < 131);
        bool local = (pressed & KEY_Y) || (touch.py >= 142 && touch.py < 178);
        if (pair) {
            setup->phase = SETUP_FORM;
            setup->field = setup->row = setup->column = 0;
            setup->notice[0] = '\0';
            return SETUP_IDLE;
        }
        if (local) {
            *endpoint = (BridgeEndpoint){.address = "127.0.0.1", .port = 8765};
            return SETUP_CONNECT;
        }
        if (first) {
            *endpoint = setup->saved;
            setup->phase = SETUP_DISCOVER;
            return SETUP_SEARCH;
        }
        return SETUP_IDLE;
    }
    if (setup->phase != SETUP_FORM) {
        return SETUP_IDLE;
    }
    if (pressed & KEY_B) {
        setup->phase = SETUP_MENU;
        memset(setup->code, 0, sizeof(setup->code));
        setup->notice[0] = '\0';
        return SETUP_IDLE;
    }
    bool toolbar = touch.py >= 180 && touch.py < 214;
    if ((pressed & KEY_SELECT) || (toolbar && touch.px >= 105 && touch.px < 210)) {
        setup->field = (setup->field + 1) % 3;
    }
    if ((repeat & KEY_Y) || (toolbar && touch.px < 105)) {
        char *target = setup->field == 0 ? setup->address : setup->field == 1 ? setup->port : setup->code;
        size_t length = strlen(target);
        if (length) {
            target[length - 1] = '\0';
        }
    }
    if (repeat & KEY_UP) setup->row = (setup->row + 3) % 4;
    else if (repeat & KEY_DOWN) setup->row = (setup->row + 1) % 4;
    if (repeat & KEY_LEFT) setup->column = (setup->column + 9) % 10;
    else if (repeat & KEY_RIGHT) setup->column = (setup->column + 1) % 10;

    bool pick = (pressed & KEY_A) != 0;
    if (touch.py >= GRID_TOP && touch.py < GRID_TOP + GRID_HEIGHT * 4 && touch.px >= 5 && touch.px < 315) {
        setup->row = (touch.py - GRID_TOP) / GRID_HEIGHT;
        setup->column = (touch.px - 5) / 31;
        pick = true;
    }
    if (pick) {
        select_key(setup, keys[setup->row][setup->column]);
    }
    if ((pressed & KEY_START) || (toolbar && touch.px >= 210)) {
        unsigned long port = strtoul(setup->port, NULL, 10);
        if (!valid_address(setup->address) || !port || port > 65535 ||
            (*setup->code && strlen(setup->code) != 20)) {
            snprintf(setup->notice, sizeof(setup->notice), "请检查地址、端口和配对码。");
            return SETUP_IDLE;
        }
        *endpoint = (BridgeEndpoint){.port = (unsigned)port};
        memcpy(endpoint->address, setup->address, sizeof(endpoint->address));
        if (!*setup->code) {
            endpoint->direct = true;
            return SETUP_CONNECT;
        }
        strcpy(endpoint->identity, "pair");
        endpoint->key_length = 20;
        memcpy(endpoint->key, setup->code, 20);
        setup->phase = SETUP_BUSY;
        setup->notice[0] = '\0';
        return SETUP_PAIR;
    }
    return SETUP_IDLE;
}

bool setup_discovered(Setup *setup, const BridgeRequest *request, BridgeEndpoint *endpoint)
{
    if (*request->error) {
        setup->phase = SETUP_MENU;
        snprintf(setup->notice, sizeof(setup->notice), "%s", request->error);
        return false;
    }
    *endpoint = request->endpoint;
    setup->phase = SETUP_CLOSED;
    setup->notice[0] = '\0';
    memcpy(setup->address, endpoint->address, sizeof(setup->address));
    snprintf(setup->port, sizeof(setup->port), "%u", endpoint->port);
    if (endpoint->key_length && (strcmp(setup->saved.address, endpoint->address) || setup->saved.port != endpoint->port)) {
        setup->saved = *endpoint;
        save_endpoint(endpoint);
    }
    return true;
}

bool setup_receive(Setup *setup, const BridgeRequest *request, BridgeEndpoint *endpoint)
{
    char identity[17];
    char hex[65];
    BridgeEndpoint paired = request->endpoint;
    const char marker[] = "3ds-codex/pair/1\n";
    if (*request->error ||
        strncmp(request->response, marker, sizeof(marker) - 1) != 0 ||
        sscanf(request->response + sizeof(marker) - 1, "%16s %64s", identity, hex) != 2 ||
        !load_credentials(&paired, identity, hex)) {
        setup->phase = SETUP_FORM;
        snprintf(setup->notice, sizeof(setup->notice), "%s",
                 *request->error ? request->error : "电脑返回了无法识别的配对信息。");
        return false;
    }
    bool saved = save_endpoint(&paired);
    setup->saved = paired;
    *endpoint = paired;
    memset(setup->code, 0, sizeof(setup->code));
    snprintf(setup->notice, sizeof(setup->notice), "%s",
             saved ? "配对成功，已保存设备凭据。" : "已配对，SD 卡保存失败。");
    setup->phase = SETUP_CLOSED;
    return true;
}

static void line(Screen screen, int x, int y, const char *value, u32 color)
{
    text(screen, x, y, value, line_end(value, screen.width - x - 8), color);
}

static void button(Screen screen, int x, int y, int width, int height, const char *label, ButtonTone tone)
{
    ui_button(screen, (Rect){x, y, width, height}, label, tone);
}

void setup_draw(Screen top, Screen bottom, const Setup *setup)
{
    ui_clear(top);
    ui_clear(bottom);
    ui_title(top, "连接 Codex", NULL, UI_MUTED);
    if (setup->phase == SETUP_DISCOVER) {
        line(top, 16, 57, "正在搜索局域网电脑…", UI_ACCENT);
        line(top, 16, 96, "请让掌机和电脑连接同一个局域网。", UI_MUTED);
        return;
    }
    if (setup->phase == SETUP_MENU) {
        line(top, 16, 62, "搜索电脑上的 Codex 桥接", UI_TEXT);
        line(top, 16, 100, "请让掌机和电脑连接同一个局域网。", UI_ACCENT);
        button(bottom, 10, 48, 300, 36, "A 重新搜索电脑", UI_PRIMARY);
        button(bottom, 10, 95, 300, 36, "X 手动连接", UI_SECONDARY);
        button(bottom, 10, 142, 300, 36, "Y 本机模拟器", UI_SECONDARY);
        if (*setup->notice) {
            line(top, 16, 150, setup->notice, UI_WARNING);
            line(top, 16, 170, line_end(setup->notice, 368), UI_WARNING);
        }
        line(bottom, 12, 220, "START 退出", UI_TEXT);
        return;
    }
    const char *labels[] = {"电脑地址", "端口", "配对码"};
    const char *values[] = {setup->address, setup->port, setup->code};
    for (unsigned i = 0; i < 3; i++) {
        int y = 52 + 38 * i;
        ui_card(top, (Rect){16, y, 368, 33}, UI_PANEL, setup->field == i ? UI_ACCENT : 0);
        line(top, 26, y + 8, labels[i], UI_ACCENT);
        line(top, 116, y + 8, values[i], UI_TEXT);
    }
    if (*setup->notice) {
        line(top, 12, 181, setup->notice, UI_WARNING);
        line(top, 12, 199, line_end(setup->notice, 380), UI_WARNING);
    }
    if (setup->phase == SETUP_BUSY) {
        line(bottom, 12, 80, "正在建立加密连接…", UI_ACCENT);
        return;
    }
    line(bottom, 8, 12, "直连可留空配对码", UI_ACCENT);
    for (unsigned row = 0; row < 4; row++) {
        for (unsigned column = 0; column < 10; column++) {
            char key[] = {keys[row][column], '\0'};
            button(bottom, 5 + 31 * column, GRID_TOP + GRID_HEIGHT * row, 29, 28, key,
                   row == setup->row && column == setup->column ? UI_PRIMARY : UI_SECONDARY);
        }
    }
    button(bottom, 5, 180, 96, 34, "Y 删除", UI_SECONDARY);
    button(bottom, 105, 180, 104, 34, "SELECT 换项", UI_SECONDARY);
    button(bottom, 215, 180, 100, 34, "START 连接", UI_PRIMARY);
    line(bottom, 8, 220, "A 输入   B 返回", UI_TEXT);
}
