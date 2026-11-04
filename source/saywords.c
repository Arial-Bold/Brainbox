/* saywords.c - see saywords.h.
 *
 * Two parts:
 *   - a tidy-up of the text before Flite sees it: markdown and emoji out,
 *     long stretched sounds ("Hmmmm", "Sooooo") shortened, and words that
 *     are better said another way swapped ("e.g." -> "for example");
 *   - extra words in the voice's dictionary, with how they're said (slang
 *     and names it guesses wrong). Sounds with no vowel ("Hmm") never reach
 *     the dictionary (Flite spells them out first), so the tidy-up
 *     respells those ("hum"). */
#include "saywords.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* -------------------------------------------------------- the dictionary */

/* word, then its sounds (Flite's US English phones; 1 marks the stressed vowel) */
static const char *const extra[][2] = {
    { "ooh", "uw1" },          { "oooh", "uw1" },          { "eek", "iy1 k" },
    { "umm", "ah1 m" },         { "err", "er1" },          { "ahem", "ax0 hh eh1 m" },
    { "hehe", "hh iy1 hh iy1" }, { "heh", "hh eh1" },      { "haha", "hh aa1 hh aa1" },
    { "hahaha", "hh aa1 hh aa1 hh aa1" }, { "aha", "aa1 hh aa1" },
    { "yay", "y ey1" },        { "yup", "y ah1 p" },       { "nah", "n aa1" },
    { "gonna", "g ah1 n ax0" }, { "wanna", "w aa1 n ax0" }, { "gotta", "g aa1 t ax0" },
    { "kinda", "k ay1 n d ax0" }, { "sorta", "s ao1 r t ax0" }, { "lotta", "l aa1 t ax0" },
    { "dunno", "d ax0 n ow1" }, { "lemme", "l eh1 m iy0" }, { "gimme", "g ih1 m iy0" },
    { "ya", "y ax0" },         { "y'all", "y ao1 l" },     { "alright", "aa1 l r ay1 t" },
    { "okay", "ow1 k ey1" },   { "ok", "ow1 k ey1" },      { "uh-oh", "ah1 ow1" },
    { "gamecube", "g ey1 m k y uw1 b" }, { "wii", "w iy1" }, { "nintendo", "n ih0 n t eh1 n d ow0" },
    { "pixel", "p ih1 k s ax0 l" }, { "gemini", "jh eh1 m ih0 n ay0" },
    { "groq", "g r aa1 k" },
};

void saywords_teach(cst_voice *v) {
    if (!v) return;
    cst_lexicon *lex = val_lexicon(feat_val(v->features, "lexicon"));
    if (!lex) return;
    cst_val *added = NULL;
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        char line[160];
        snprintf(line, sizeof(line), "%s : %s", extra[i][0], extra[i][1]);
        cst_val *e = cst_lex_make_entry(lex, line);
        if (e) added = cons_val(e, added);
    }
    /* (in front of any already there, so these win) */
    if (lex->lex_addenda) added = val_append(val_reverse(added), lex->lex_addenda);
    lex->lex_addenda = added;
}

/* ----------------------------------------------------------- the tidy-up */

/* whole words (any case) said differently; the replacement as Flite should read it */
static const char *const swaps[][2] = {
    { "e.g.", "for example" }, { "eg.", "for example" }, { "i.e.", "that is" }, { "etc.", "et cetera" },
    { "etc", "et cetera" },    { "vs.", "versus" },      { "vs", "versus" },    { "approx.", "about" },
    { "dr.", "doctor" },       { "prof.", "professor" }, { "st.", "saint" },    { "mt.", "mount" },
    { "y'know", "you know" },  { "ya know", "you know" }, { "c'mon", "come on" }, { "'cause", "because" },
    { "cuz", "because" },      { "gotcha", "got you" },  { "lol", "ha ha" },    { "btw", "by the way" },
    { "fyi", "for your information" }, { "imo", "in my opinion" }, { "tbh", "to be honest" },
    { "omg", "oh my goodness" }, { "w/", "with " },     { "&", " and " },      { "+", " plus " },
    { "=", " equals " },       { "\xb0" "f", " degrees Fahrenheit" }, { "\xb0" "c", " degrees Celsius" }, { "\xb0", " degrees" },
    { "mm-hmm", "mum hum" },   { "mmhmm", "mum hum" },       { "uh-huh", "uh huh" }, { "hm-m", "hmm" },
    { "zzz", "snore" },
    /* sounds with no vowel (or a doubled one), which Flite would spell out
     * letter by letter before its dictionary is even asked: spelled the way
     * they sound instead */
    { "hmm", "hum" },          { "hm", "hum" },          { "mhm", "mum hum" },     { "mm", "mum" },
    { "ahh", "ah" },           { "aah", "ah" },          { "ohh", "oh" },         { "uhh", "uh" },
    { "shh", "shush" },        { "psst", "hey" },        { "brr", "burr" },       { "tsk", "tisk" },
    { "pfft", "puff" },        { "soo", "so" },         { "noo", "no" },
    { "yess", "yes" },        { "chatgpt", "chat G P T" }, { "ai", "A I" },
};

static bool word_char(unsigned char c) { return isalnum(c) || c == '\'' || c >= 0xC0; }

/* does s start with the swap word w (any case), standing alone as a word? */
static size_t starts_word(const char *s, const char *w, bool after_word) {
    if (after_word && word_char((unsigned char)w[0])) return 0;
    size_t n = strlen(w);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != (unsigned char)w[i]) return 0;
    if (word_char((unsigned char)w[n - 1]) && word_char((unsigned char)s[n])) return 0;
    return n;
}

/* first: long stretched sounds ("Hmmmm", "Sooooo", "Ahhhhh") to at most two of a letter */
static void unstretch(const char *in, char *out, size_t cap) {
    size_t o = 0;
    int run = 0;
    char prev = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        if (isalpha((unsigned char)*p) && tolower((unsigned char)*p) == tolower((unsigned char)prev)) {
            if (++run >= 2) continue;
        } else run = 0;
        out[o++] = *p;
        prev = *p;
    }
    out[o] = 0;
}

void saywords_tidy(const char *in, char *out, size_t cap) {
    static char tmp[4096];
    unstretch(in, tmp, sizeof(tmp));
    size_t o = 0;
    bool prev_word = false;
    for (const unsigned char *p = (const unsigned char *)tmp; *p && o + 24 < cap;) {
        unsigned char c = *p;
        /* swaps */
        bool swapped = false;
        for (size_t i = 0; i < sizeof(swaps) / sizeof(swaps[0]); i++) {
            size_t n = starts_word((const char *)p, swaps[i][0], prev_word);
            if (!n) continue;
            const char *r = swaps[i][1];
            size_t rn = strlen(r);
            if (o + rn + 2 >= cap) break;
            memcpy(out + o, r, rn);
            /* keep a capital at the start of a sentence */
            if (isupper(c) && islower((unsigned char)out[o])) out[o] = (char)toupper((unsigned char)out[o]);
            o += rn;
            p += n;
            prev_word = word_char((unsigned char)r[rn - 1]);
            swapped = true;
            break;
        }
        if (swapped) continue;
        /* markdown and the like, said as nothing */
        if (c == '*' || c == '_' || c == '#' || c == '`' || c == '~' || c == '|' || c == '^') { p++; continue; }
        if (c == 0x95 || c == 0x97) { out[o++] = ','; out[o++] = ' '; p++; prev_word = false; continue; }   /* a bullet, a long dash */
        if (c == 0x85) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; p++; prev_word = false; continue; }   /* ... */
        out[o++] = (char)c;
        prev_word = word_char(c);
        p++;
    }
    out[o] = 0;
}
