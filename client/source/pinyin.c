#include "pinyin.h"
#include <stdint.h>
#include <string.h>

extern const uint8_t pinyin_data[];

enum { ENTRY_BYTES = 12, MAX_DICTIONARY_DIGITS = 24, MAX_COMPLETION_DIGITS = 4 };

typedef struct {
    const char *words;
    uint32_t weight;
} DictionaryEntry;

typedef struct {
    const char *word;
    unsigned end;
    unsigned pieces;
    unsigned weight;
} PinyinSplit;

typedef struct {
    const char *word;
    uint64_t score;
} Suggestion;

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static const uint8_t *index_entry(unsigned index)
{
    return pinyin_data + 4 + index * ENTRY_BYTES;
}

static const char *pool_string(const uint8_t *offset)
{
    return (const char *)index_entry(read_u32(pinyin_data)) + read_u32(offset);
}

static unsigned lower_bound(const char *key)
{
    unsigned low = 0;
    unsigned high = read_u32(pinyin_data);
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        if (strcmp(pool_string(index_entry(mid)), key) < 0) low = mid + 1;
        else high = mid;
    }
    return low;
}

static DictionaryEntry dictionary_lookup(const char *digits, unsigned length)
{
    if (!length || length > MAX_DICTIONARY_DIGITS) return (DictionaryEntry){0};
    char key[MAX_DICTIONARY_DIGITS + 1];
    memcpy(key, digits, length);
    key[length] = '\0';

    unsigned index = lower_bound(key);
    if (index == read_u32(pinyin_data)) return (DictionaryEntry){0};
    const uint8_t *entry = index_entry(index);
    if (strcmp(pool_string(entry), key)) return (DictionaryEntry){0};
    return (DictionaryEntry){pool_string(entry + 4), read_u32(entry + 8)};
}

static void combine_pinyin(const char *digits, unsigned length, char *output)
{
    PinyinSplit split[PINYIN_CAPACITY] = {0};
    for (unsigned start = length; start-- > 0;) {
        unsigned farthest = length - start > MAX_DICTIONARY_DIGITS ? start + MAX_DICTIONARY_DIGITS : length;
        for (unsigned end = farthest; end > start; end--) {
            if ((start == 0 && end == length) || (end != length && !split[end].word)) continue;
            DictionaryEntry entry = dictionary_lookup(digits + start, end - start);
            if (!entry.words) continue;
            unsigned pieces = split[end].pieces + 1;
            unsigned weight = split[end].weight + (entry.weight ? 32 - __builtin_clz(entry.weight) : 0);
            PinyinSplit *best = &split[start];
            if (!best->word || pieces < best->pieces || (pieces == best->pieces && weight > best->weight)) {
                *best = (PinyinSplit){entry.words, end, pieces, weight};
            }
        }
    }
    if (!split[0].word) return;
    for (unsigned start = 0; start < length; start = split[start].end) {
        size_t bytes = strlen(split[start].word);
        memcpy(output, split[start].word, bytes);
        output += bytes;
    }
    *output = '\0';
}

static void suggest(Suggestion *best, const char *word, uint64_t score, const char *excluded)
{
    if (!word || (excluded && !strcmp(word, excluded))) return;
    if (!best->word || score > best->score) *best = (Suggestion){word, score};
}

static const char *completion(const char *digits, unsigned length, const char *excluded)
{
    if (length < 2 || length >= MAX_DICTIONARY_DIGITS) return NULL;
    Suggestion best = {0};
    unsigned count = read_u32(pinyin_data);
    for (unsigned i = lower_bound(digits); i < count; i++) {
        const uint8_t *entry = index_entry(i);
        const char *key = pool_string(entry);
        if (strncmp(key, digits, length)) break;
        unsigned extra = strlen(key) - length;
        if (!extra || extra > MAX_COMPLETION_DIGITS) continue;
        uint64_t weight = (uint64_t)read_u32(entry + 8) << (2 * (MAX_COMPLETION_DIGITS - extra));
        suggest(&best, pool_string(entry + 4), weight, excluded);
    }
    return best.word;
}

static void score_edit(Suggestion *best, const char *key, unsigned length, unsigned priority,
                       const char *excluded)
{
    DictionaryEntry entry = dictionary_lookup(key, length);
    suggest(best, entry.words, (uint64_t)entry.weight * priority, excluded);
}

static bool nearby(char first, char second)
{
    int a = first - '1';
    int b = second - '1';
    int dx = a % 3 - b % 3;
    int dy = a / 3 - b / 3;
    return dx * dx + dy * dy == 1;
}

static const char *correction(const char *digits, unsigned length, const char *excluded)
{
    if (length < 4 || length > MAX_DICTIONARY_DIGITS + 1) return NULL;
    Suggestion best = {0};
    char key[MAX_DICTIONARY_DIGITS + 2];
    for (unsigned i = 0; i < length; i++) {
        memcpy(key, digits, i);
        memcpy(key + i, digits + i + 1, length - i - 1);
        score_edit(&best, key, length - 1, 1, excluded);

        memcpy(key + i + 1, digits + i + 1, length - i - 1);
        if (i + 1 < length && digits[i] != digits[i + 1]) {
            key[i] = digits[i + 1];
            key[i + 1] = digits[i];
            score_edit(&best, key, length, 3, excluded);
            key[i + 1] = digits[i + 1];
        }
        for (char digit = '2'; digit <= '9'; digit++) {
            if (digit == digits[i]) continue;
            key[i] = digit;
            score_edit(&best, key, length, nearby(digit, digits[i]) ? 6 : 2, excluded);
        }

        if (length >= MAX_DICTIONARY_DIGITS) continue;
        memcpy(key + i + 1, digits + i, length - i);
        for (char digit = '2'; digit <= '9'; digit++) {
            key[i] = digit;
            score_edit(&best, key, length + 1, 3, excluded);
        }
    }
    return best.word;
}

static void emit(PinyinPage *page, unsigned *skip, unsigned *used, const char *word,
                 unsigned consumed, PinyinKind kind)
{
    if (!word) return;
    if (*skip) {
        (*skip)--;
    } else if (*used < PINYIN_CANDIDATES) {
        page->items[(*used)++] = (PinyinCandidate){kind == PINYIN_COMBINED ? NULL : word, consumed, kind};
    } else {
        page->more = true;
    }
}

PinyinPage pinyin_candidates(const char *digits, unsigned page_number)
{
    PinyinPage page = {0};
    unsigned length = strlen(digits);
    if (!length) return page;
    unsigned skip = page_number * PINYIN_CANDIDATES;
    unsigned used = 0;
    DictionaryEntry exact = dictionary_lookup(digits, length);
    const char *first = exact.words;
    if (!first) {
        combine_pinyin(digits, length, page.combined);
        if (*page.combined) first = page.combined;
    }
    const char *completed = completion(digits, length, first);
    const char *corrected = correction(digits, length, first);
    if (completed && corrected && !strcmp(completed, corrected)) corrected = NULL;

    emit(&page, &skip, &used, first, length, exact.words ? PINYIN_EXACT : PINYIN_COMBINED);
    emit(&page, &skip, &used, completed, length, PINYIN_COMPLETION);
    emit(&page, &skip, &used, corrected, length, PINYIN_CORRECTION);

    const char *words = exact.words;
    unsigned consumed = length;
    if (words) {
        words += strlen(words) + 1;
    } else {
        while (!words && consumed > 1) {
            consumed--;
            words = dictionary_lookup(digits, consumed).words;
        }
    }
    for (const char *word = words; word && *word && !page.more; word += strlen(word) + 1) {
        if ((first && !strcmp(word, first)) || (completed && !strcmp(word, completed)) ||
            (corrected && !strcmp(word, corrected))) continue;
        emit(&page, &skip, &used, word, consumed, exact.words ? PINYIN_EXACT : PINYIN_PREFIX);
    }
    return page;
}

const char *pinyin_word(const PinyinPage *page, unsigned index)
{
    return page->items[index].kind == PINYIN_COMBINED ? page->combined : page->items[index].word;
}
