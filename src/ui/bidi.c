#include "bidi.h"

/* Bidi classes needed by UI strings. AL is folded into R (Arabic is the only
 * AL script in practice, and treating it as R gives the same visual result
 * for everything except the ES/CS rules between Arabic numbers). */
typedef enum {
    BC_L, BC_R, BC_EN, BC_AN, BC_ES, BC_ET, BC_CS, BC_NSM, BC_BN, BC_WS, BC_ON
} bclass;

static bclass classify(osa_rune r) {
    /* Arabic + Hebrew + Thaana + Syriac blocks are strongly RTL */
    if (r >= 0x0590 && r <= 0x05FF) return BC_R;
    if (r >= 0x0600 && r <= 0x06FF) {
        if (r >= 0x0660 && r <= 0x0669) return BC_AN;
        if (r >= 0x06F0 && r <= 0x06F9) return BC_EN;
        if (r == 0x06DD || r == 0x06DE || r == 0x060C) return BC_CS;
        return BC_R;
    }
    if (r >= 0x0700 && r <= 0x074F) return BC_R;
    if (r >= 0x0750 && r <= 0x077F) return BC_R;
    if (r >= 0x0780 && r <= 0x07BF) return BC_R;
    if (r >= 0x08A0 && r <= 0x08FF) return BC_R;
    if (r >= 0xFB50 && r <= 0xFDFF) return BC_R;
    if (r >= 0xFE70 && r <= 0xFEFF) return BC_R;
    if (r >= 0x10800 && r <= 0x10FFF) return BC_R;
    if ((r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z')) return BC_L;
    if (r >= 0x00C0 && r <= 0x02AF) return BC_L;
    if (r >= 0x0370 && r <= 0x052F) return BC_L;
    if (r >= 0x2E80 && r <= 0x9FFF) return BC_L;
    if (r >= 0xAC00 && r <= 0xD7AF) return BC_L;
    if (r >= '0' && r <= '9') return BC_EN;
    if (r == '+' || r == '-') return BC_ES;
    if (r == '/' || r == ':' || r == ',' || r == '.' || r == 0x060C) return BC_CS;
    if (r == '#' || r == '$' || r == '%' || r == 0x00A0) return BC_ET;
    if (r == '\t' || r == ' ' || r == '\n' || r == '\r') return BC_WS;
    if (r >= 0x0300 && r <= 0x036F) return BC_NSM;
    if (r == 0x200C || r == 0x200D || r == 0x0000) return BC_BN;
    if (r == 0x200B) return BC_WS;
    return BC_ON;
}

static inline bool bidi_is_rtl_class(bclass c) { return c == BC_R || c == BC_AN; }

bool bidi_is_paragraph_rtl(const char *utf8) {
    if (!utf8) return false;
    size_t len = strlen(utf8), i = 0;
    while (i < len) {
        osa_rune r = 0;
        int n = osa_utf8_decode(utf8 + i, len - i, &r);
        if (n <= 0) break;
        bclass c = classify(r);
        if (c == BC_L) return false;
        if (c == BC_R) return true;
        i += (size_t)n;
    }
    return false;
}

#define MAXL 125

int bidi_paragraph(const char *utf8, bidi_run *runs, int max_runs, bool *out_para_rtl) {
    if (out_para_rtl) *out_para_rtl = false;
    if (!utf8 || !*utf8 || max_runs <= 0) return 0;
    size_t len = strlen(utf8);

    /* ---------------- decode ---------------- */
    static osa_rune cp[BIDI_MAX_RUNES];
    static size_t   off[BIDI_MAX_RUNES + 1];   /* off[n] = end */
    static bclass   cls[BIDI_MAX_RUNES];
    int n = 0;
    size_t i = 0;
    while (i < len && n < BIDI_MAX_RUNES) {
        osa_rune r = 0;
        int k = osa_utf8_decode(utf8 + i, len - i, &r);
        if (k <= 0) k = 1;
        cp[n] = r; off[n] = i; cls[n] = classify(r);
        n++;
        i += (size_t)k;
    }
    off[n] = i;

    /* ---------------- explicit levels (X1..X8) ---------------- */
    static int  levels[BIDI_MAX_RUNES];
    static int  stack_lvl[MAXL + 2];
    static uint8_t stack_ovr[MAXL + 2];     /* 0 neutral, 1 L, 2 R */
    int sp = 0;
    int para = 0;
    /* P2/P3 */
    for (int j = 0; j < n; j++) {
        if (cls[j] == BC_L) { para = 0; break; }
        if (cls[j] == BC_R) { para = 1; break; }
    }
    if (out_para_rtl) *out_para_rtl = (para == 1);
    stack_lvl[0] = para; stack_ovr[0] = 0;
    int cur_lvl = para;
    uint8_t cur_ovr = 0;
    for (int j = 0; j < n; j++) {
        osa_rune r = cp[j];
        if (r == 0x202A || r == 0x202B || r == 0x202D || r == 0x202E) {  /* LRE RLE LRO RLO */
            int nl = (r == 0x202A || r == 0x202D) ? ((cur_lvl + 2) & ~1) : ((cur_lvl + 1) | 1);
            uint8_t no = 0;
            if (r == 0x202D) no = 1;
            if (r == 0x202E) no = 2;
            levels[j] = cur_lvl;
            if (nl <= MAXL && sp < MAXL) {
                sp++;
                stack_lvl[sp] = nl; stack_ovr[sp] = no;
                cur_lvl = nl; cur_ovr = no;
            }
            cls[j] = BC_BN;
            continue;
        }
        if (r == 0x202C) {                                              /* PDF */
            levels[j] = cur_lvl;
            if (sp > 0) { cur_lvl = stack_lvl[sp - 1]; cur_ovr = stack_ovr[sp - 1]; sp--; }
            cls[j] = BC_BN;
            continue;
        }
        if (cur_ovr == 1) { levels[j] = cur_lvl; cls[j] = BC_L; }
        else if (cur_ovr == 2) { levels[j] = cur_lvl; cls[j] = BC_R; }
        else levels[j] = cur_lvl;
    }

    /* convenience macros over resolved (working) classes */
    static bclass w[BIDI_MAX_RUNES];
    memcpy(w, cls, (size_t)n * sizeof(bclass));
    /* CR/LF behave as paragraph separators for our purposes -> B; fold to WS */
    for (int j = 0; j < n; j++) if (cp[j] == '\n' || cp[j] == '\r') w[j] = BC_WS;

    /* ---------------- X10: isolate runs ---------------- */
    /* Runs are maximal sequences of characters with the same embedding level.
       W/N/I rules are applied per run (the UBA allows per-run resolution as an
       optimization; we keep it simple and correct for UI text). */
    int run_start = 0;
    while (run_start < n) {
        int lv = levels[run_start];
        int run_end = run_start;
        while (run_end < n && levels[run_end] == lv) run_end++;
        /* --- W1: NSM -> previous class --- */
        for (int j = run_start; j < run_end; j++) {
            if (w[j] == BC_NSM) {
                if (j > run_start) w[j] = w[j - 1];
                else w[j] = (lv & 1) ? BC_R : BC_L;
            }
        }
        /* --- W2: EN after AL -> AN (numbers following Arabic letters) --- */
        for (int j = run_start; j < run_end; j++) {
            if (w[j] == BC_EN) {
                int a = j - 1;
                while (a >= run_start && w[a] == BC_NSM) a--;
                if (a >= run_start && w[a] == BC_R) w[j] = BC_AN;
            }
        }
        /* --- W3: AL -> R (folded at classification) --- */
        /* --- W4: ES between EN -> EN ; CS between EN -> EN ; CS between AN -> AN --- */
        for (int j = run_start + 1; j + 1 < run_end; j++) {
            if (w[j] == BC_ES && w[j-1] == BC_EN && w[j+1] == BC_EN) w[j] = BC_EN;
            else if (w[j] == BC_CS && w[j-1] == BC_EN && w[j+1] == BC_EN) w[j] = BC_EN;
            else if (w[j] == BC_CS && w[j-1] == BC_AN && w[j+1] == BC_AN) w[j] = BC_AN;
        }
        /* --- W5: ET adjacent to EN -> EN --- */
        for (int j = run_start; j < run_end; j++) {
            if (w[j] != BC_ET) continue;
            int a = j - 1, b = j + 1;
            while (a >= run_start && w[a] == BC_ET) a--;
            while (b < run_end && w[b] == BC_ET) b++;
            if ((a >= run_start && w[a] == BC_EN) || (b < run_end && w[b] == BC_EN)) w[j] = BC_EN;
        }
        /* --- W6: remaining separators -> ON --- */
        for (int j = run_start; j < run_end; j++)
            if (w[j] == BC_ES || w[j] == BC_ET || w[j] == BC_CS) w[j] = BC_ON;
        /* --- W7: EN with L before (and no R before) -> L --- */
        bool saw_r = false;
        for (int j = run_start; j < run_end; j++) {
            if (w[j] == BC_R || w[j] == BC_AN) saw_r = true;
            else if (w[j] == BC_EN && !saw_r) w[j] = BC_L;
        }
        /* --- N1/N2: neutrals --- */
        bclass embed = (lv & 1) ? BC_R : BC_L;
        for (int j = run_start; j < run_end; j++) {
            if (w[j] != BC_WS && w[j] != BC_ON && w[j] != BC_BN) continue;
            int a = j - 1, b = j;
            while (a >= run_start && (w[a] == BC_WS || w[a] == BC_ON || w[a] == BC_BN)) a--;
            while (b < run_end && (w[b] == BC_WS || w[b] == BC_ON || w[b] == BC_BN)) b++;
            bclass before = (a >= run_start) ? w[a] : embed;
            bclass after  = (b < run_end)    ? w[b] : embed;
            /* N1: EN/AN are treated as R when adjacent to neutrals */
            if (before == BC_EN || before == BC_AN) before = BC_R;
            if (after == BC_EN || after == BC_AN) after = BC_R;
            bclass res = (before == after) ? before : embed;
            for (int t = a + 1; t < b; t++) w[t] = res;
        }
        /* --- I1/I2: implicit levels --- */
        for (int j = run_start; j < run_end; j++) {
            if (lv & 1) {
                if (w[j] == BC_L) levels[j] = lv + 1;
                else if (w[j] == BC_EN || w[j] == BC_AN) levels[j] = lv + 1;
                else levels[j] = lv;
            } else {
                if (w[j] == BC_R) levels[j] = lv + 1;
                else if (w[j] == BC_AN) levels[j] = lv + 2;
                else if (w[j] == BC_EN) levels[j] = lv + 2;
                else levels[j] = lv;
            }
            if (levels[j] > MAXL) levels[j] = MAXL;
        }
        run_start = run_end;
    }

    /* ---------------- L1: trailing whitespace back to paragraph level ------- */
    for (int j = 0; j < n; j++) {
        if (cp[j] == '\n' || cp[j] == '\r') {
            int e = j;
            while (e >= 0 && (cp[e] == '\n' || cp[e] == '\r' || cls[e] == BC_WS)) {
                levels[e] = para;
                e--;
            }
        }
    }
    {
        int e = n - 1;
        while (e >= 0 && (cls[e] == BC_WS || cls[e] == BC_BN)) { levels[e] = para; e--; }
    }

    /* ---------------- L2: reorder level runs ----------------
     * Level runs are logical, maximal sequences of equal level. HarfBuzz
     * shapes each run internally (with its own direction), so reordering must
     * happen at run granularity - not per character - or Arabic letters lose
     * their joining forms. */
    static int run_off[BIDI_MAX_RUNS];      /* logical run start (rune index) */
    static int run_len[BIDI_MAX_RUNS];
    static int run_lvl[BIDI_MAX_RUNS];
    int nr = 0;
    int j = 0;
    while (j < n && nr < BIDI_MAX_RUNS) {
        int lev = levels[j];
        int k = j;
        while (k < n && levels[k] == lev) k++;
        run_off[nr] = j;
        run_len[nr] = k - j;
        run_lvl[nr] = lev;
        nr++;
        j = k;
    }
    int maxl = 0, minodd = MAXL + 1;
    for (int t = 0; t < nr; t++) {
        if (run_lvl[t] > maxl) maxl = run_lvl[t];
        if ((run_lvl[t] & 1) && run_lvl[t] < minodd) minodd = run_lvl[t];
    }
    if (minodd > maxl) minodd = maxl + 1;
    for (int L = maxl; L >= minodd; L--) {
        int t = 0;
        while (t < nr) {
            if (run_lvl[t] >= L) {
                int k = t;
                while (k < nr && run_lvl[k] >= L) k++;
                for (int a = t, b = k - 1; a < b; a++, b--) {
                    int to = run_off[a], tl = run_len[a], tv = run_lvl[a];
                    run_off[a] = run_off[b]; run_len[a] = run_len[b]; run_lvl[a] = run_lvl[b];
                    run_off[b] = to; run_len[b] = tl; run_lvl[b] = tv;
                }
                t = k;
            } else t++;
        }
    }

    /* ---------------- emit visual runs ---------------- */
    int emitted = 0;
    for (int t = 0; t < nr && emitted < max_runs; t++) {
        size_t start = off[run_off[t]];
        size_t endb = off[run_off[t] + run_len[t]];
        /* drop zero-width runs (explicit formatting characters) */
        if (endb <= start) continue;
        runs[emitted].start = start;
        runs[emitted].len = endb - start;
        runs[emitted].level = run_lvl[t];
        runs[emitted].rtl = (run_lvl[t] & 1) != 0;
        emitted++;
    }
    return emitted;
}
