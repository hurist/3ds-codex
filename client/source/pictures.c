#include "pictures.h"

#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static const Rect smaller = {16, 8, 140, 34};
static const Rect larger = {164, 8, 140, 34};
static const Rect previous = {16, 52, 140, 34};
static const Rect next = {164, 52, 140, 34};
static const Rect drag_pad = {16, 97, 288, 83};
static const Rect back = {16, 190, 288, 35};

void pictures_load_thumbs(Pictures *pictures, const char *index, const char *body)
{
    pictures->count = 0;
    const char *item = strchr(index, '|');
    if (!item) return;
    item++;
    size_t body_length = strlen(body);
    while (*item && pictures->count < PICTURE_COUNT) {
        const char *end = strchr(item, ';');
        if (!end) end = item + strlen(item);
        PictureThumb *thumb = &pictures->thumbs[pictures->count];
        int prefix = 0;
        if (sscanf(item, "%u,%32[0-9a-f],%u,%u,%n", &thumb->offset, thumb->token,
                   &thumb->width, &thumb->height, &prefix) == 4 &&
            prefix > 0 && item + prefix <= end && strlen(thumb->token) == 32 && thumb->offset < body_length) {
            const char *line = body + thumb->offset;
            size_t written = 0;
            if (!thumb->width && !thumb->height && item + prefix == end && !strncmp(line, "图片附件", strlen("图片附件"))) {
                pictures->count++;
            } else if (thumb->width && thumb->width <= THUMB_WIDTH && thumb->height && thumb->height <= THUMB_HEIGHT &&
                !strncmp(line, " \n \n", 4) && mbedtls_base64_decode(thumb->pixels, sizeof(thumb->pixels), &written,
                                      (const u8 *)item + prefix, end - item - prefix) == 0 &&
                written == thumb->width * thumb->height * 2) {
                pictures->count++;
            }
        }
        item = *end ? end + 1 : end;
    }
}

int pictures_at(const Pictures *pictures, unsigned offset)
{
    for (unsigned i = 0; i < pictures->count; i++) {
        unsigned start = pictures->thumbs[i].offset;
        if (offset >= start && offset <= start + (pictures->thumbs[i].width ? 4 : 0)) return i;
    }
    return -1;
}

static void blit(Screen screen, const u8 *pixels, unsigned width, unsigned height,
                 Rect area, Rect clip)
{
    int left = area.x > clip.x ? area.x : clip.x;
    int top = area.y > clip.y ? area.y : clip.y;
    int right = area.x + area.w;
    int bottom = area.y + area.h;
    if (right > clip.x + clip.w) right = clip.x + clip.w;
    if (bottom > clip.y + clip.h) bottom = clip.y + clip.h;
    unsigned row_offset[240];
    for (int row = top; row < bottom; row++) {
        row_offset[row - top] = ((row - area.y) * height / area.h) * width * 2;
    }
    for (int column = left; column < right; column++) {
        unsigned source_x = (column - area.x) * width / area.w * 2;
        u8 *out = screen.pixels + (column * screen.height + screen.height - 1 - top) * 3;
        for (int row = top; row < bottom; row++, out -= 3) {
            unsigned source = row_offset[row - top] + source_x;
            unsigned value = pixels[source] | pixels[source + 1] << 8;
            unsigned blue = value & 31;
            unsigned green = (value >> 5) & 63;
            unsigned red = value >> 11;
            out[0] = (blue << 3) | (blue >> 2);
            out[1] = (green << 2) | (green >> 4);
            out[2] = (red << 3) | (red >> 2);
        }
    }
}

void pictures_draw_thumb(const PictureThumb *thumb, Screen screen, int x, int y, unsigned slice)
{
    int origin = y - 18 * (int)slice + (THUMB_HEIGHT - (int)thumb->height) / 2;
    blit(screen, thumb->pixels, thumb->width, thumb->height,
         (Rect){x, origin, thumb->width, thumb->height}, (Rect){x, y, THUMB_WIDTH, 18});
}

bool pictures_load(Pictures *pictures, const char *response, size_t length)
{
    const char marker[] = "3ds-codex/image/2\n";
    unsigned width, height;
    int header = 0;
    if (length < sizeof(marker) || strncmp(response, marker, sizeof(marker) - 1) ||
        sscanf(response + sizeof(marker) - 1, "%u %u%n", &width, &height, &header) != 2 ||
        !width || width > PREVIEW_WIDTH || !height || height > PREVIEW_HEIGHT) return false;
    size_t offset = sizeof(marker) - 1 + header;
    if (offset >= length || response[offset++] != '\n') return false;
    uLongf size = width * height * 2;
    if (uncompress(pictures->pixels, &size, (const u8 *)response + offset, length - offset) != Z_OK ||
        size != width * height * 2) return false;
    pictures->width = width;
    pictures->height = height;
    pictures->ready = true;
    return true;
}

void pictures_prepare(Pictures *pictures, unsigned selected)
{
    pictures->selected = selected;
    pictures->open = true;
    pictures->ready = false;
    pictures->zoom = 1;
    pictures->pan_x = pictures->pan_y = 0;
    pictures->dragging = false;
    pictures->error[0] = '\0';
}

static Rect image_area(const Pictures *pictures, unsigned zoom)
{
    int width = pictures->width;
    int height = pictures->height;
    if (width > 400 || height > 192) {
        if (width * 192 > height * 400) {
            height = height * 400 / width;
            width = 400;
        } else {
            width = width * 192 / height;
            height = 192;
        }
    }
    width = (width ? width : 1) * zoom;
    height = (height ? height : 1) * zoom;
    return (Rect){(400 - width) / 2 + pictures->pan_x, 42 + (192 - height) / 2 + pictures->pan_y,
                  width, height};
}

static int clamp_pan(int pan, int size, int viewport)
{
    int limit = size > viewport ? (size - viewport + 1) / 2 : 0;
    return pan < -limit ? -limit : pan > limit ? limit : pan;
}

bool pictures_input(Pictures *pictures, u32 pressed, u32 held, touchPosition touch, int *change)
{
    if ((pressed & KEY_B) || ((pressed & KEY_TOUCH) && ui_hit(back, touch))) {
        pictures->open = false;
        return true;
    }
    if ((pressed & KEY_L) || ((pressed & KEY_TOUCH) && ui_hit(previous, touch))) {
        if (pictures->selected > 0) *change = pictures->selected - 1;
    }
    if ((pressed & KEY_R) || ((pressed & KEY_TOUCH) && ui_hit(next, touch))) {
        if (pictures->selected + 1 < pictures->count) *change = pictures->selected + 1;
    }
    if (!pictures->ready) return false;

    int x = pictures->pan_x;
    int y = pictures->pan_y;
    unsigned zoom = pictures->zoom;
    if (((pressed & KEY_X) || ((pressed & KEY_TOUCH) && ui_hit(larger, touch))) && zoom < 3) zoom++;
    if (((pressed & KEY_Y) || ((pressed & KEY_TOUCH) && ui_hit(smaller, touch))) && zoom > 1) zoom--;
    if (held & KEY_LEFT) x += 3;
    if (held & KEY_RIGHT) x -= 3;
    if (held & KEY_UP) y += 3;
    if (held & KEY_DOWN) y -= 3;
    if (!(held & KEY_TOUCH)) pictures->dragging = false;
    if ((pressed & KEY_TOUCH) && ui_hit(drag_pad, touch)) {
        pictures->dragging = true;
        pictures->drag_x = touch.px;
        pictures->drag_y = touch.py;
    } else if (pictures->dragging) {
        x += (int)touch.px - pictures->drag_x;
        y += (int)touch.py - pictures->drag_y;
        pictures->drag_x = touch.px;
        pictures->drag_y = touch.py;
    }

    Rect area = image_area(pictures, zoom);
    x = clamp_pan(x, area.w, 400);
    y = clamp_pan(y, area.h, 192);
    bool moved = x != pictures->pan_x || y != pictures->pan_y || zoom != pictures->zoom;
    pictures->pan_x = x;
    pictures->pan_y = y;
    pictures->zoom = zoom;
    return moved;
}

void pictures_draw(const Pictures *pictures, Screen top, Screen bottom)
{
    ui_clear(top);
    ui_clear(bottom);
    char page[24];
    snprintf(page, sizeof(page), "%u / %u", pictures->selected + 1, pictures->count);
    ui_title(top, "图片预览", page, UI_ACCENT);
    if (pictures->ready) {
        blit(top, pictures->pixels, pictures->width, pictures->height,
             image_area(pictures, pictures->zoom), (Rect){0, 42, 400, 192});
    } else {
        ui_label(top, 20, 73, 360, *pictures->error ? pictures->error : "正在加载图片…", UI_MUTED);
    }
    ui_button(bottom, smaller, "Y 缩小", pictures->ready && pictures->zoom > 1 ? UI_SECONDARY : UI_DISABLED);
    ui_button(bottom, larger, "X 放大", pictures->ready && pictures->zoom < 3 ? UI_PRIMARY : UI_DISABLED);
    ui_button(bottom, previous, "L 上一张", pictures->selected > 0 ? UI_SECONDARY : UI_DISABLED);
    ui_button(bottom, next, "R 下一张", pictures->selected + 1 < pictures->count ? UI_SECONDARY : UI_DISABLED);
    ui_card(bottom, drag_pad, UI_PANEL, 0);
    ui_label(bottom, 28, 130, 260, pictures->zoom > 1 ? "拖动这里查看细节" : "放大后可拖动图片", UI_ACCENT);
    ui_button(bottom, back, "B 返回对话", UI_SECONDARY);
}
