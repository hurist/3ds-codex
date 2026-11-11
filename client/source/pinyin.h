#ifndef PINYIN_H
#define PINYIN_H

#include <stdbool.h>

#define PINYIN_CAPACITY 49
#define PINYIN_CANDIDATES 4

typedef enum {
    PINYIN_EXACT,
    PINYIN_COMBINED,
    PINYIN_COMPLETION,
    PINYIN_CORRECTION,
    PINYIN_PREFIX,
} PinyinKind;

typedef struct {
    const char *word;
    unsigned consumed;
    PinyinKind kind;
} PinyinCandidate;

typedef struct {
    PinyinCandidate items[PINYIN_CANDIDATES];
    char combined[PINYIN_CAPACITY * 3];
    bool more;
} PinyinPage;

PinyinPage pinyin_candidates(const char *digits, unsigned page);
const char *pinyin_word(const PinyinPage *page, unsigned index);

#endif
