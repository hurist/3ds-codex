#ifndef SELECTION_H
#define SELECTION_H

#include "composer.h"

typedef struct {
    unsigned label;
    unsigned body;
    unsigned end;
} MessageRange;

typedef struct {
    const char *body;
    const char **lines;
    unsigned count;
    const MessageRange *messages;
    unsigned message_count;
} SelectionPage;

typedef struct {
    const char *cursor;
    const char *anchor;
    unsigned top;
    const char *notice;
} Selection;

const char *selection_copy_message(const SelectionPage *page, const char *cursor,
                                   char clipboard[MESSAGE_CAPACITY]);
unsigned selection_line(const SelectionPage *page, const char *cursor);
bool selection_input(Selection *selection, const SelectionPage *page, u32 pressed, u32 repeat,
                     touchPosition touch, char clipboard[MESSAGE_CAPACITY]);
void selection_draw(Screen top, Screen bottom, const char *title, const Selection *selection,
                    const SelectionPage *page);

#endif
