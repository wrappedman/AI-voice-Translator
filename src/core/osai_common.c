#include "osai_common.h"
#include <time.h>
#include <math.h>

/* ============================== arena ============================== */
struct osa_arena_chunk {
    osa_arena_chunk *next;
    size_t cap, used;
    /* data follows */
};

#define ARENA_ALIGN 16
#define ARENA_DEFAULT (64 * 1024)

static osa_arena_chunk *arena_new_chunk(size_t cap) {
    if (cap < ARENA_DEFAULT) cap = ARENA_DEFAULT;
    osa_arena_chunk *c = (osa_arena_chunk *)calloc(1, sizeof(osa_arena_chunk) + cap);
    if (!c) return NULL;
    c->cap = cap;
    return c;
}

void osa_arena_init(osa_arena *a) { memset(a, 0, sizeof *a); }

void *osa_arena_alloc_raw(osa_arena *a, size_t n) {
    n = (n + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    if (!n) n = ARENA_ALIGN;
    osa_arena_chunk *c = a->head;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > ARENA_DEFAULT ? n : ARENA_DEFAULT;
        osa_arena_chunk *nc = arena_new_chunk(cap);
        if (!nc) return NULL;
        nc->next = a->head;
        a->head = nc;
        a->cap += cap;
        c = nc;
    }
    void *p = (char *)(c + 1) + c->used;
    c->used += n;
    a->used += n;
    return p;
}

void *osa_arena_alloc(osa_arena *a, size_t n) {
    void *p = osa_arena_alloc_raw(a, n);
    if (p) memset(p, 0, n);
    return p;
}

char *osa_arena_strndup(osa_arena *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *p = (char *)osa_arena_alloc_raw(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
char *osa_arena_strdup(osa_arena *a, const char *s) {
    return s ? osa_arena_strndup(a, s, strlen(s)) : NULL;
}

char *osa_arena_printf(osa_arena *a, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char *p = (char *)osa_arena_alloc_raw(a, (size_t)n + 1);
    if (p) vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void osa_arena_free(osa_arena *a) {
    osa_arena_chunk *c = a->head;
    while (c) { osa_arena_chunk *n = c->next; free(c); c = n; }
    memset(a, 0, sizeof *a);
}

size_t osa_arena_bytes(const osa_arena *a) { return a ? a->used : 0; }

/* ============================== buffer ============================== */
void osa_buf_init(osa_buf *b) { memset(b, 0, sizeof *b); }
void osa_buf_free(osa_buf *b) { free(b->data); memset(b, 0, sizeof *b); }
void osa_buf_reset(osa_buf *b) { b->len = 0; if (b->data) b->data[0] = 0; b->oom = false; }

void osa_buf_grow(osa_buf *b, size_t need) {
    if (b->oom) return;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) { b->oom = true; return; }
    b->data = p;
    b->cap = cap;
}

void osa_buf_putn(osa_buf *b, const void *p, size_t n) {
    osa_buf_grow(b, b->len + n + 1);
    if (b->oom) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

void osa_buf_puts(osa_buf *b, const char *s) {
    if (!s) s = "";
    osa_buf_putn(b, s, strlen(s));
}
void osa_buf_putc(osa_buf *b, char c) { osa_buf_putn(b, &c, 1); }

void osa_buf_printf(osa_buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        osa_buf_grow(b, b->len + (size_t)n + 1);
        if (!b->oom) {
            vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
            b->len += (size_t)n;
        }
    }
    va_end(ap2);
}

char *osa_buf_detach(osa_buf *b) {
    if (!b->data) { char *p = (char *)calloc(1, 1); b->len = b->cap = 0; return p; }
    char *p = b->data;
    memset(b, 0, sizeof *b);
    return p;
}

/* ============================== strings ============================== */
bool osa_streq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static inline char lower_(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool osa_strieq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) { if (lower_(*a) != lower_(*b)) return false; a++; b++; }
    return *a == *b;
}

bool osa_starts(const char *s, const char *p) {
    if (!s || !p) return false;
    return strncmp(s, p, strlen(p)) == 0;
}
bool osa_ends(const char *s, const char *p) {
    if (!s || !p) return false;
    size_t ls = strlen(s), lp = strlen(p);
    return ls >= lp && memcmp(s + ls - lp, p, lp) == 0;
}

const char *osa_strstr_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    size_t n = strlen(needle);
    if (!n) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && lower_(hay[i]) == lower_(needle[i])) i++;
        if (i == n) return hay;
    }
    return NULL;
}

void osa_strlcpy(char *dst, const char *src, size_t cap) {
    if (!dst || !cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

char *osa_strdup(const char *s) { return s ? osa_strndup(s, strlen(s)) : NULL; }

char *osa_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *osa_trim_inplace(char *s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
    return s;
}

int osa_split(char *s, char sep, char **out, int maxout) {
    int n = 0;
    if (!s || maxout <= 0) return 0;
    out[n++] = s;
    for (char *p = s; *p && n < maxout; p++) {
        if (*p == sep) { *p = 0; out[n++] = p + 1; }
    }
    return n;
}

/* ============================== JSON ============================== */
char *osa_json_escape(osa_arena *a, const char *s) {
    osa_buf b; osa_buf_init(&b);
    osa_buf_putc(&b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"':  osa_buf_puts(&b, "\\\""); break;
                case '\\': osa_buf_puts(&b, "\\\\"); break;
                case '\n': osa_buf_puts(&b, "\\n"); break;
                case '\r': osa_buf_puts(&b, "\\r"); break;
                case '\t': osa_buf_puts(&b, "\\t"); break;
                default:
                    if (*p < 0x20) osa_buf_printf(&b, "\\u%04x", *p);
                    else osa_buf_putc(&b, (char)*p);
            }
        }
    }
    osa_buf_putc(&b, '"');
    char *raw = osa_buf_detach(&b);
    char *out = osa_arena_strdup(a, raw);
    free(raw);
    return out;
}

/* find `"key"` followed by ':' — returns pointer just after ':' */
static const char *json_find(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t kl = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"'))) {
        const char *k = p + 1;
        if (strncmp(k, key, kl) == 0) {
            const char *q = k + kl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == ':') {
                    q++;
                    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                    return q;
                }
            }
        }
        p++;
    }
    return NULL;
}

bool osa_json_get_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json_find(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u') {
                /* minimal: decode BMP escape as '?' for non-ASCII, keep ASCII */
                unsigned v = 0;
                if (p[1] && p[2] && p[3] && p[4]) {
                    for (int i = 1; i <= 4; i++) {
                        char h = p[i]; v <<= 4;
                        v |= (unsigned)(h >= '0' && h <= '9' ? h - '0' :
                                        h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                        h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                    }
                    p += 4;
                }
                if (v && n + 1 < cap) out[n++] = (char)(v < 128 ? (int)v : '?');
                p++;
                continue;
            }
            out[n++] = c;
        } else out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return true;
}

bool osa_json_get_bool(const char *json, const char *key, bool *out) {
    const char *p = json_find(json, key);
    if (!p) return false;
    if (osa_starts(p, "true"))  { *out = true;  return true; }
    if (osa_starts(p, "false")) { *out = false; return true; }
    if (*p == '1') { *out = true; return true; }
    if (*p == '0') { *out = false; return true; }
    return false;
}

bool osa_json_get_num(const char *json, const char *key, double *out) {
    const char *p = json_find(json, key);
    if (!p) return false;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

/* ============================== utf-8 ============================== */
int osa_utf8_decode(const char *s, size_t len, osa_rune *out) {
    if (!s || !len) { if (out) *out = 0; return 0; }
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { if (out) *out = p[0]; return 1; }
    int n; osa_rune r;
    if ((p[0] & 0xE0) == 0xC0) { n = 2; r = p[0] & 0x1F; }
    else if ((p[0] & 0xF0) == 0xE0) { n = 3; r = p[0] & 0x0F; }
    else if ((p[0] & 0xF8) == 0xF0) { n = 4; r = p[0] & 0x07; }
    else { if (out) *out = 0xFFFD; return 1; }
    if ((size_t)n > len) { if (out) *out = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { if (out) *out = 0xFFFD; return 1; }
        r = (r << 6) | (p[i] & 0x3F);
    }
    if (out) *out = r;
    return n;
}

int osa_utf8_encode(osa_rune r, char out[4]) {
    if (r < 0x80) { out[0] = (char)r; return 1; }
    if (r < 0x800) { out[0] = (char)(0xC0 | (r >> 6)); out[1] = (char)(0x80 | (r & 0x3F)); return 2; }
    if (r < 0x10000) {
        out[0] = (char)(0xE0 | (r >> 12)); out[1] = (char)(0x80 | ((r >> 6) & 0x3F));
        out[2] = (char)(0x80 | (r & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (r >> 18)); out[1] = (char)(0x80 | ((r >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((r >> 6) & 0x3F)); out[3] = (char)(0x80 | (r & 0x3F));
    return 4;
}

bool osa_utf8_valid(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        osa_rune r; int n = osa_utf8_decode(s + i, len - i, &r);
        if (n <= 0) return false;
        if (r == 0xFFFD && (unsigned char)s[i] >= 0x80) return false;
        i += (size_t)n;
    }
    return true;
}

size_t osa_utf8_count(const char *s) {
    size_t n = 0;
    for (const char *p = s; p && *p; n++) {
        unsigned char c = (unsigned char)*p;
        p += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    }
    return n;
}

bool osa_rune_is_rtl(osa_rune r) {
    /* Arabic 0600-06FF, Arabic Supplement 0750-077F, Arabic Ext-A 08A0-08FF,
       Arabic Presentation A/B FB50-FDFF, FE70-FEFF, Hebrew 0590-05FF,
       Thaana 0780-07BF, Syriac 0700-074F */
    return (r >= 0x0600 && r <= 0x06FF) || (r >= 0x0750 && r <= 0x077F) ||
           (r >= 0x08A0 && r <= 0x08FF) || (r >= 0xFB50 && r <= 0xFDFF) ||
           (r >= 0xFE70 && r <= 0xFEFF) || (r >= 0x0590 && r <= 0x05FF) ||
           (r >= 0x0700 && r <= 0x074F) || (r >= 0x0780 && r <= 0x07BF);
}

/* ============================== SHA-256 ============================== */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x,n) (((x) >> (n)) | ((x) << (32-(n))))

void osa_sha256_init(osa_sha256 *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->buflen = 0;
}

static void sha256_block(osa_sha256 *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void osa_sha256_update(osa_sha256 *c, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    if (c->buflen) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
    while (n >= 64) { sha256_block(c, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->buflen = n; }
}

void osa_sha256_final(osa_sha256 *c, uint8_t digest[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    osa_sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) osa_sha256_update(c, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i*8));
    osa_sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)(c->h[i] >> 24);
        digest[i*4+1] = (uint8_t)(c->h[i] >> 16);
        digest[i*4+2] = (uint8_t)(c->h[i] >> 8);
        digest[i*4+3] = (uint8_t)(c->h[i]);
    }
}

char *osa_hex(const void *data, size_t n, char *out) {
    static const char *H = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = H[p[i] >> 4];
        out[i*2+1] = H[p[i] & 15];
    }
    out[n*2] = 0;
    return out;
}

void osa_sha256_hex(const void *data, size_t n, char out[65]) {
    osa_sha256 c; osa_sha256_init(&c);
    osa_sha256_update(&c, data, n);
    uint8_t d[32]; osa_sha256_final(&c, d);
    osa_hex(d, 32, out);
}

void osa_sha256_file(const char *path, char out[65], uint64_t *size_out) {
    out[0] = 0;
    if (size_out) *size_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    osa_sha256 c; osa_sha256_init(&c);
    static uint8_t tmp[1 << 16];
    size_t n; uint64_t total = 0;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) { osa_sha256_update(&c, tmp, n); total += n; }
    fclose(f);
    uint8_t d[32]; osa_sha256_final(&c, d);
    osa_hex(d, 32, out);
    if (size_out) *size_out = total;
}

uint32_t osa_crc32(uint32_t seed, const void *data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = ~seed;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint64_t osa_hash64(const void *data, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* ============================== base64 ============================== */
size_t osa_base64_encode(const uint8_t *in, size_t n, char *out) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];  out[o++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

size_t osa_base64_decode(const char *in, size_t n, uint8_t *out) {
    int8_t T[256];
    memset(T, -1, sizeof T);
    const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) T[(unsigned char)A[i]] = (int8_t)i;
    size_t o = 0; uint32_t v = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int8_t d = T[(unsigned char)in[i]];
        if (d < 0) continue;
        v = (v << 6) | (uint32_t)d; bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(v >> bits); }
    }
    return o;
}

/* ============================== time ============================== */

/* A pinned clock, used by the snapshot renderer.
 *
 * The screenshots are checked into the repository and CI fails when they
 * drift, so anything derived from "now" — history timestamps above all — has
 * to be pinned or the images change every run. When g_fixed_ms >= 0 the rest
 * of the program sees a constant wall clock. */
static int64_t g_fixed_ms = -1;

void osa_time_freeze(int64_t epoch_ms) { g_fixed_ms = epoch_ms; }
bool osa_time_is_frozen(void) { return g_fixed_ms >= 0; }

int64_t osa_now_ms(void) {
    if (g_fixed_ms >= 0) return g_fixed_ms;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t osa_now_us(void) {
    if (g_fixed_ms >= 0) return g_fixed_ms * 1000;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
void osa_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
void osa_iso_time(int64_t epoch_ms, char out[32]) {
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    struct tm *lt = localtime(&t);
    if (lt) tmv = *lt;
#else
    localtime_r(&t, &tmv);
#endif
    /* Manual formatting: strftime("%Y-%m-%d") follows LC_TIME, which would
     * make stored timestamps locale-dependent and the snapshots unstable. */
    /* clamp the year so the compiler can see the result always fits */
    int y = tmv.tm_year + 1900;
    if (y < 0) y = 0;
    if (y > 9999) y = 9999;
    snprintf(out, 32, "%04d-%02d-%02d %02d:%02d:%02d", y,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}
int64_t osa_iso_parse(const char *s) {
    int y=0,mo=0,d=0,h=0,mi=0,se=0;
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &y,&mo,&d,&h,&mi,&se) < 3) return 0;
    struct tm tmv; memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se;
    return (int64_t)mktime(&tmv) * 1000;
}

void osa_uuid(char out[37]) {
    static uint64_t ctr = 0;
    uint8_t b[16];
    uint64_t t = (uint64_t)osa_now_us();
    ctr++;
    uint64_t h1 = osa_hash64(&t, 8) ^ (ctr * 0x9E3779B97F4A7C15ull);
    uint64_t h2 = osa_hash64(b, 0) ^ (t << 1) ^ (ctr << 32);
    if (h2 == 0) h2 = 0xA5A5A5A5A5A5A5A5ull;
    memcpy(b, &h1, 8); memcpy(b + 8, &h2, 8);
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

void osa_short_id(char out[13]) {
    uint64_t t = (uint64_t)osa_now_ms();
    uint64_t h = osa_hash64(&t, 8);
    static const char *A = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < 12; i++) { out[i] = A[h & 31]; h >>= 5; }
    out[12] = 0;
}

/* ============================== math ============================== */
float osa_db_to_lin(float db) { return powf(10.0f, db / 20.0f); }
float osa_lin_to_db(float lin) { return lin <= 0.0f ? -120.0f : 20.0f * log10f(lin); }

/* ============================== logging ============================== */
static osa_log_sink g_sink = NULL;
static void *g_sink_ud = NULL;
static osai_log_level g_level = OSAI_LOG_INFO;

void osa_log_set_sink(osa_log_sink sink, void *ud) { g_sink = sink; g_sink_ud = ud; }
void osa_log_set_level(osai_log_level lvl) { g_level = lvl; }

void osa_log(osai_log_level lvl, const char *tag, const char *fmt, ...) {
    if (lvl < g_level) return;
    static const char *N[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char line[1100];
    snprintf(line, sizeof line, "[%s] %-6s %s: %s", "", N[lvl & 3], tag ? tag : "-", msg);
    if (g_sink) g_sink(lvl, line, g_sink_ud);
#ifndef OSAI_NO_STDERR_LOG
    if (g_level <= OSAI_LOG_DEBUG) {
        fputs(line, stderr);
        fputc('\n', stderr);
    }
#endif
}
