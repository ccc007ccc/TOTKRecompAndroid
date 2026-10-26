// See i18n.h.
#include "i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

#include "imgui.h"
#ifdef __ANDROID__
#include <vector>
// (ImGui's own TrueType reader, a private copy: a phone font it can't read leaves the whole atlas empty - HyperOS's
// NotoSansCJK did, and the start screen crashed uploading it)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#endif

extern "C" {
#include "../settings.h"
}

#ifdef __ANDROID__
typedef const char *const Pair[2];
static const char *const kPt[][2] = {
#include "i18n_pt.inc"
};
static const char *const kEs[][2] = {
#include "i18n_es.inc"
};
static const char *const kFr[][2] = {
#include "i18n_fr.inc"
};
static const char *const kDe[][2] = {
#include "i18n_de.inc"
};
static const char *const kIt[][2] = {
#include "i18n_it.inc"
};
static const char *const kNl[][2] = {
#include "i18n_nl.inc"
};
static const char *const kRu[][2] = {
#include "i18n_ru.inc"
};
static const char *const kJa[][2] = {
#include "i18n_ja.inc"
};
static const char *const kKo[][2] = {
#include "i18n_ko.inc"
};
static const char *const kZhHans[][2] = {
#include "i18n_zh_hans.inc"
};
static const char *const kZhHant[][2] = {
#include "i18n_zh_hant.inc"
};

// The menu language's table (settings_menu_language), NULL for English
struct Table { const char *code; Pair *rows; size_t n; };
#define TABLE(code, t) {code, t, sizeof t / sizeof *t}
static const Table kTables[] = {TABLE("pt", kPt), TABLE("es", kEs), TABLE("fr", kFr), TABLE("de", kDe), TABLE("it", kIt),
                                TABLE("nl", kNl), TABLE("ru", kRu), TABLE("ja", kJa), TABLE("ko", kKo),
                                TABLE("zh-Hans", kZhHans), TABLE("zh-Hant", kZhHant)};
static const Table *menu_table(void) {
    static const Table *t = [] {
        const char *code = settings_menu_language();
        for (const Table &k : kTables)
            if (!strcmp(k.code, code)) return &k;
        return (const Table *)NULL;
    }();
    return t;
}
#endif

const char *tr(const char *en) {
#ifdef __ANDROID__
    const Table *t = en && *en ? menu_table() : NULL;
    if (!t) return en;
    static const auto *map = [t] {  // (built once, thread-safe: the start screen's callbacks run on Java's threads)
        auto *m = new std::unordered_map<std::string, const char *>();
        for (size_t i = 0; i < t->n; i++) m->emplace(t->rows[i][0], t->rows[i][1]);
        return m;
    }();
    auto it = map->find(en);
    return it == map->end() ? en : it->second;
#else
    return en;
#endif
}

std::string ui_upper(const char *s) {
    std::string t = s ? s : "";
    for (size_t i = 0; i < t.size(); i++) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x80) t[i] = (char)toupper(c);
#ifdef __ANDROID__  // (the desktop's menus are English: bytes as before)
        else if (c == 0xC3 && i + 1 < t.size()) {  // U+00E0..U+00FE: the lowercase accented letters (but ÷, U+00F7)
            unsigned char d = (unsigned char)t[i + 1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) t[i + 1] = (char)(d - 0x20);
            i++;
        } else if ((c == 0xD0 || c == 0xD1) && i + 1 < t.size()) {  // Cyrillic: а..п (D0 B0-BF), р..я (D1 80-8F), ё
            unsigned char d = (unsigned char)t[i + 1];
            if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) t[i + 1] = (char)(d - 0x20);
            else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) t[i] = (char)0xD0, t[i + 1] = (char)(d + 0x20);
            else if (c == 0xD1 && d == 0x91) t[i] = (char)0xD0, t[i + 1] = (char)0x81;
            i++;
        } else if (c >= 0xC0) {  // (other letters as they are: skip the rest of the sequence)
            while (i + 1 < t.size() && ((unsigned char)t[i + 1] & 0xC0) == 0x80) i++;
        }
#endif
    }
    return t;
}

void i18n_merge_fonts(ImFontAtlas *atlas, float size_px) {
#ifdef __ANDROID__
    // The shipped Baloo 2 has Latin only: Russian, Japanese, Korean and Chinese come from the phone's own fonts, merged
    // into the font just added, with the glyphs the menu language's table uses (a few hundred, not whole scripts: the
    // atlas stays small)
    const Table *t = menu_table();
    if (!t) return;
    const char *code = t->code;
    bool cjk = !strcmp(code, "ja") || !strcmp(code, "ko") || !strncmp(code, "zh", 2);
    if (!cjk && strcmp(code, "ru")) return;
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        for (size_t i = 0; i < t->n; i++) b.AddText(t->rows[i][1]);
        b.AddText("0123456789 %.,:;!?()[]/-+x");
        b.BuildRanges(&ranges);
    }
    // Candidates, the language's own first: Xiaomi's MiSans (HyperOS), then Google's NotoSansCJK collection (Japanese,
    // Korean, Simplified, Traditional Chinese faces in that order: Android's fonts.xml); each checked with ImGui's reader
    // (font_ok) - variable CFF2 fonts it can't read
    int face = !strcmp(code, "ko") ? 1 : !strcmp(code, "zh-Hans") ? 2 : !strcmp(code, "zh-Hant") ? 3 : 0;
    static const char *const own[] = {"/system/fonts/MiSansJapaneseVF.ttf", "/system/fonts/MiSansKoreanVF.ttf",
                                      "/system/fonts/MiSansVF.ttf", "/system/fonts/MiSansTCVF.ttf"};
    const char *cjk_fonts[] = {cjk ? own[face] : "", "/system/fonts/NotoSansCJK-Regular.ttc",
                               "/system/fonts/NotoSansCJK-Medium.ttc", "/system/fonts/NotoSerifCJK-Regular.ttc",
                               "/system/fonts/DroidSansFallbackFull.ttf", "/system/fonts/DroidSansFallback.ttf"};
    static const char *const cyr_fonts[] = {"/system/fonts/RobotoStatic-Medium.ttf", "/system/fonts/Roboto-Medium.ttf",
                                            "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf",
                                            "/system/fonts/NotoSans-Regular.ttf"};
    const char *const *list = cjk ? cjk_fonts : cyr_fonts;
    size_t n = cjk ? sizeof cjk_fonts / sizeof *cjk_fonts : sizeof cyr_fonts / sizeof *cyr_fonts;
    // a letter of the language the font must have (あ, 한, 中, 中, Я)
    int probe = !strcmp(code, "ja") ? 0x3042 : !strcmp(code, "ko") ? 0xD55C : cjk ? 0x4E2D : 0x042F;
    static int chosen = -2;  // (the same for every size: checked once)
    static const char *chosen_path;
    if (chosen == -2) {
        chosen = -1;
        for (size_t i = 0; i < n && chosen < 0; i++) {
            FILE *f = *list[i] ? fopen(list[i], "rb") : NULL;  // (a missing file is an assertion in ImGui)
            if (!f) continue;
            std::vector<unsigned char> data;
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0) data.resize((size_t)size);
            bool read = size > 0 && fread(data.data(), 1, data.size(), f) == data.size();
            fclose(f);
            int fno = strstr(list[i], ".ttc") ? face : 0, off = read ? stbtt_GetFontOffsetForIndex(data.data(), fno) : -1;
            stbtt_fontinfo info;
            if (off >= 0 && stbtt_InitFont(&info, data.data(), off) && stbtt_FindGlyphIndex(&info, probe) != 0) {
                chosen = fno;
                chosen_path = list[i];
            } else {
                fprintf(stderr, "[ui] menus in %s: %s can't be used\n", code, list[i]);
            }
        }
    }
    if (chosen >= 0) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontNo = chosen;
        const char *const path = chosen_path;
        cfg.OversampleH = 1;  // (big CJK glyph sets: half the atlas of the default 2)
        cfg.PixelSnapH = true;
        // (Baloo 2 runs small for its size: the system fonts at ~60% of it match its letters' height)
        atlas->AddFontFromFileTTF(path, size_px * 0.6f, &cfg, ranges.Data);
        static bool logged;
        if (!logged) logged = true, fprintf(stderr, "[ui] menus in %s: glyphs from %s\n", code, path);
        return;
    }
    fprintf(stderr, "[ui] menus in %s: no system font with its letters\n", code);
#else
    (void)atlas, (void)size_px;
#endif
}
