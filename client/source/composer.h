#ifndef COMPOSER_H
#define COMPOSER_H

#include "display.h"
#include "pinyin.h"

#include <stdbool.h>

#define MESSAGE_CAPACITY 8193

typedef struct {
    char text[MESSAGE_CAPACITY];
    unsigned length;
    unsigned caret;
    unsigned row;
    unsigned column;
    unsigned candidate_page;
    char digits[PINYIN_CAPACITY];
    unsigned symbol_page;
    bool symbols;
    bool latin;
    bool edit_caret;
    const char *notice;
    bool shift;
    bool keypad_focus;
} Composer;

typedef enum { COMPOSER_CONTINUE, COMPOSER_BACK, COMPOSER_SEND, COMPOSER_VOICE } ComposeAction;

bool composer_insert_text(Composer *draft, const char *value);
ComposeAction composer_input(Composer *draft, u32 pressed, u32 repeat, const char *clipboard);
void composer_draw(const Composer *draft, Screen top, Screen bottom, const char *title, bool follow_up, bool can_paste, bool answering, u32 held);

#endif
