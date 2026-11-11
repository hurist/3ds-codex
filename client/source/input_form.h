#ifndef INPUT_FORM_H
#define INPUT_FORM_H

#include "network.h"
#include "ui.h"

#define FORM_QUESTIONS 16
#define FORM_OPTIONS 32

typedef struct {
    const char *label;
    const char *description;
} FormOption;

typedef struct {
    const char *header;
    const char *prompt;
    FormOption options[FORM_OPTIONS];
    unsigned option_count;
    bool free_text;
    int answer;
    char text[MESSAGE_CAPACITY];
} FormQuestion;

typedef struct {
    char token[33];
    char payload[RESPONSE_CAPACITY];
    FormQuestion questions[FORM_QUESTIONS];
    unsigned count;
    unsigned page;
    unsigned cursor;
    unsigned scroll;
    const char *notice;
} InputForm;

typedef enum { FORM_CONTINUE, FORM_BACK, FORM_EDIT, FORM_SUBMIT } FormAction;

bool form_load(InputForm *form, const char *response);
FormAction form_input(InputForm *form, u32 pressed, u32 repeat, touchPosition touch);
void form_draw(const InputForm *form, Screen top, Screen bottom, bool busy, const char *error, bool retry);
bool form_write(const InputForm *form, char *target, size_t capacity);

#endif
