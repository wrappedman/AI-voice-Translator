#include "nn.h"
#include <math.h>

#define NN_MAGIC 0x4F53414Du   /* "OSAM" */

static float rnd_uniform(uint32_t *state) {
    /* xorshift32 -> [-1,1] */
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return (float)((double)x / 2147483648.0 - 1.0);
}

nn_model *nn_create(int n_in, int n_hidden, int n_out, int layers, int n_h2) {
    if (n_in <= 0 || n_out <= 0 || n_hidden <= 0) return NULL;
    if (layers < 1 || layers > 2) layers = 1;
    if (layers == 2 && n_h2 <= 0) n_h2 = n_hidden;
    nn_model *m = (nn_model *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->n_in = n_in; m->n_hidden = n_hidden; m->n_out = n_out;
    m->layers = layers; m->n_h2 = layers == 2 ? n_h2 : 0;
    m->w1 = (float *)calloc((size_t)n_hidden * n_in, sizeof(float));
    m->b1 = (float *)calloc((size_t)n_hidden, sizeof(float));
    m->w2 = layers == 2 ? (float *)calloc((size_t)n_h2 * n_hidden, sizeof(float)) : NULL;
    m->b2 = layers == 2 ? (float *)calloc((size_t)n_h2, sizeof(float)) : NULL;
    int last = layers == 2 ? n_h2 : n_hidden;
    m->w3 = (float *)calloc((size_t)n_out * last, sizeof(float));
    m->b3 = (float *)calloc((size_t)n_out, sizeof(float));
    m->magic = NN_MAGIC;
    if (!m->w1 || !m->b1 || !m->w3 || !m->b3) { nn_free(m); return NULL; }
    return m;
}

void nn_free(nn_model *m) {
    if (!m) return;
    free(m->w1); free(m->b1); free(m->w2); free(m->b2); free(m->w3); free(m->b3);
    free(m);
}

void nn_init_random(nn_model *m, uint32_t seed) {
    uint32_t st = seed ? seed : 0x9E3779B9u;
    int last = m->layers == 2 ? m->n_h2 : m->n_hidden;
    float s1 = sqrtf(2.0f / (float)m->n_in);
    float s2 = sqrtf(2.0f / (float)m->n_hidden);
    float s3 = sqrtf(2.0f / (float)last);
    for (int i = 0; i < m->n_hidden * m->n_in; i++) m->w1[i] = rnd_uniform(&st) * s1;
    for (int i = 0; i < m->n_hidden; i++) m->b1[i] = 0.01f;
    if (m->layers == 2) {
        for (int i = 0; i < m->n_h2 * m->n_hidden; i++) m->w2[i] = rnd_uniform(&st) * s2;
        for (int i = 0; i < m->n_h2; i++) m->b2[i] = 0.01f;
    }
    for (int i = 0; i < m->n_out * last; i++) m->w3[i] = rnd_uniform(&st) * s3;
    for (int i = 0; i < m->n_out; i++) m->b3[i] = 0;
}

/* forward pass with cached activations; returns loss. Buffers are caller-sized:
 * h1[n_hidden], h2[n_h2], logits[n_out] */
static double forward_full(const nn_model *m, const float *x,
                           float *h1, float *h2, float *logits, float *probs) {
    for (int j = 0; j < m->n_hidden; j++) {
        const float *w = m->w1 + (size_t)j * m->n_in;
        float a = m->b1[j];
        for (int i = 0; i < m->n_in; i++) a += w[i] * x[i];
        h1[j] = a > 0 ? a : 0;
    }
    const float *last_in = h1;
    int last_n = m->n_hidden;
    if (m->layers == 2) {
        for (int j = 0; j < m->n_h2; j++) {
            const float *w = m->w2 + (size_t)j * m->n_hidden;
            float a = m->b2[j];
            for (int i = 0; i < m->n_hidden; i++) a += w[i] * h1[i];
            h2[j] = a > 0 ? a : 0;
        }
        last_in = h2;
        last_n = m->n_h2;
    }
    for (int j = 0; j < m->n_out; j++) {
        const float *w = m->w3 + (size_t)j * last_n;
        float a = m->b3[j];
        for (int i = 0; i < last_n; i++) a += w[i] * last_in[i];
        logits[j] = a;
    }
    if (probs) {
        memcpy(probs, logits, (size_t)m->n_out * sizeof(float));
        nn_softmax(probs, m->n_out);
    }
    return 0;
}

double nn_forward(const nn_model *m, const float *x, float *out) {
    float *h1 = (float *)malloc((size_t)m->n_hidden * sizeof(float));
    float *h2 = m->layers == 2 ? (float *)malloc((size_t)m->n_h2 * sizeof(float)) : NULL;
    if (!h1 || (m->layers == 2 && !h2)) { free(h1); free(h2); return -1; }
    forward_full(m, x, h1, h2, out, NULL);
    free(h1); free(h2);
    return 0;
}

void nn_softmax(float *v, int n) {
    if (n <= 0) return;
    float mx = v[0];
    for (int i = 1; i < n; i++) if (v[i] > mx) mx = v[i];
    double sum = 0;
    for (int i = 0; i < n; i++) { v[i] = expf(v[i] - mx); sum += v[i]; }
    if (sum < 1e-12) return;
    for (int i = 0; i < n; i++) v[i] = (float)(v[i] / sum);
}

int nn_argmax(const float *v, int n) {
    if (n <= 0) return -1;
    int best = 0;
    for (int i = 1; i < n; i++) if (v[i] > v[best]) best = i;
    return best;
}

/* ---------------------------- Adam ---------------------------- */
typedef struct { float *g, *m, *v; } opt_state;

static void adam_apply(float *w, const float *g, float *m, float *v, int n, int t,
                       float lr, float b1, float b2, float eps) {
    float bc1 = 1.0f - powf(b1, (float)t);
    float bc2 = 1.0f - powf(b2, (float)t);
    for (int i = 0; i < n; i++) {
        m[i] = b1 * m[i] + (1 - b1) * g[i];
        v[i] = b2 * v[i] + (1 - b2) * g[i] * g[i];
        float mh = m[i] / bc1, vh = v[i] / bc2;
        w[i] -= lr * mh / (sqrtf(vh) + eps);
    }
}

/* adam_m/adam_v are one contiguous buffer of 2*param_count floats supplied by
 * the caller (cached in the trainer). Layout: [m all params][v all params]. */
static int param_count(const nn_model *m) {
    int last = m->layers == 2 ? m->n_h2 : m->n_hidden;
    return m->n_hidden * m->n_in + m->n_hidden +
           (m->layers == 2 ? m->n_h2 * m->n_hidden + m->n_h2 : 0) +
           m->n_out * last + m->n_out;
}

int nn_param_count(const nn_model *m) { return param_count(m); }

double nn_train_sample(nn_model *m, const float *x, int label, float lr,
                       float *adam_mv, int *adam_t, float clip) {
    int pc = param_count(m);
    if (!adam_mv) return -1;
    float *am = adam_mv, *av = adam_mv + pc;
    float *h1 = (float *)malloc((size_t)m->n_hidden * sizeof(float));
    float *h2 = m->layers == 2 ? (float *)malloc((size_t)m->n_h2 * sizeof(float)) : NULL;
    float *logits = (float *)malloc((size_t)m->n_out * sizeof(float));
    float *probs = (float *)malloc((size_t)m->n_out * sizeof(float));
    float *dlog = (float *)calloc((size_t)m->n_out, sizeof(float));
    if (!h1 || !logits || !probs || !dlog || (m->layers == 2 && !h2)) {
        free(h1); free(h2); free(logits); free(probs); free(dlog);
        return -1;
    }
    forward_full(m, x, h1, h2, logits, probs);
    double loss = -log(fmax((double)probs[label], 1e-9));
    for (int i = 0; i < m->n_out; i++) dlog[i] = probs[i] - (i == label ? 1.0f : 0.0f);
    if (clip > 0) {
        float n2 = 0;
        for (int i = 0; i < m->n_out; i++) n2 += dlog[i] * dlog[i];
        if (n2 > clip * clip) {
            float s = clip / sqrtf(n2);
            for (int i = 0; i < m->n_out; i++) dlog[i] *= s;
        }
    }
    int last_n = m->layers == 2 ? m->n_h2 : m->n_hidden;
    const float *last_in = m->layers == 2 ? h2 : h1;
    float *grad_w3 = (float *)malloc((size_t)m->n_out * last_n * sizeof(float));
    float *grad_b3 = (float *)malloc((size_t)m->n_out * sizeof(float));
    float *dlast = (float *)calloc((size_t)last_n, sizeof(float));
    if (!grad_w3 || !grad_b3 || !dlast) {
        free(h1); free(h2); free(logits); free(probs); free(dlog);
        free(grad_w3); free(grad_b3); free(dlast);
        return -1;
    }
    for (int j = 0; j < m->n_out; j++) {
        grad_b3[j] = dlog[j];
        for (int i = 0; i < last_n; i++) {
            grad_w3[(size_t)j * last_n + i] = dlog[j] * last_in[i];
            dlast[i] += dlog[j] * m->w3[(size_t)j * last_n + i];
        }
    }
    /* offsets into the Adam buffers must match the parameter layout */
    int off = 0;
    (*adam_t)++;
    int t = *adam_t;
    adam_apply(m->w3, grad_w3, am + off, av + off, m->n_out * last_n, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_out * last_n;
    adam_apply(m->b3, grad_b3, am + off, av + off, m->n_out, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_out;
    if (m->layers == 2) {
        float *dh2 = dlast;
        float *grad_w2 = (float *)malloc((size_t)m->n_h2 * m->n_hidden * sizeof(float));
        float *grad_b2 = (float *)malloc((size_t)m->n_h2 * sizeof(float));
        float *dh1 = (float *)calloc((size_t)m->n_hidden, sizeof(float));
        if (!grad_w2 || !grad_b2 || !dh1) {
            free(grad_w2); free(grad_b2); free(dh1); free(h1); free(h2); free(logits);
            free(probs); free(dlog); free(grad_w3); free(grad_b3); free(dlast);
            return -1;
        }
        for (int j = 0; j < m->n_h2; j++) {
            float a = dh2[j];
            if (h2[j] <= 0) a = 0;
            grad_b2[j] = a;
            for (int i = 0; i < m->n_hidden; i++) {
                grad_w2[(size_t)j * m->n_hidden + i] = a * h1[i];
                dh1[i] += a * m->w2[(size_t)j * m->n_hidden + i];
            }
        }
        adam_apply(m->w2, grad_w2, am + off, av + off, m->n_h2 * m->n_hidden, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_h2 * m->n_hidden;
        adam_apply(m->b2, grad_b2, am + off, av + off, m->n_h2, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_h2;
        float *grad_w1 = (float *)malloc((size_t)m->n_hidden * m->n_in * sizeof(float));
        float *grad_b1 = (float *)malloc((size_t)m->n_hidden * sizeof(float));
        for (int j = 0; j < m->n_hidden; j++) {
            float a = dh1[j];
            if (h1[j] <= 0) a = 0;
            grad_b1[j] = a;
            for (int i = 0; i < m->n_in; i++) grad_w1[(size_t)j * m->n_in + i] = a * x[i];
        }
        adam_apply(m->w1, grad_w1, am + off, av + off, m->n_hidden * m->n_in, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_hidden * m->n_in;
        adam_apply(m->b1, grad_b1, am + off, av + off, m->n_hidden, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_hidden;
        free(grad_w1); free(grad_b1); free(grad_w2); free(grad_b2); free(dh1);
    } else {
        float *dh1 = (float *)calloc((size_t)m->n_hidden, sizeof(float));
        float *grad_w1 = (float *)malloc((size_t)m->n_hidden * m->n_in * sizeof(float));
        float *grad_b1 = (float *)malloc((size_t)m->n_hidden * sizeof(float));
        if (!dh1 || !grad_w1 || !grad_b1) {
            free(dh1); free(grad_w1); free(grad_b1); free(h1); free(h2); free(logits);
            free(probs); free(dlog); free(grad_w3); free(grad_b3); free(dlast);
            return -1;
        }
        for (int i = 0; i < m->n_hidden; i++) dh1[i] = dlast[i] * (h1[i] > 0 ? 1.0f : 0.0f);
        for (int j = 0; j < m->n_hidden; j++) {
            grad_b1[j] = dh1[j];
            for (int i = 0; i < m->n_in; i++) grad_w1[(size_t)j * m->n_in + i] = dh1[j] * x[i];
        }
        adam_apply(m->w1, grad_w1, am + off, av + off, m->n_hidden * m->n_in, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_hidden * m->n_in;
        adam_apply(m->b1, grad_b1, am + off, av + off, m->n_hidden, t, lr, 0.9f, 0.999f, 1e-8f); off += m->n_hidden;
        free(dh1); free(grad_w1); free(grad_b1);
    }
    free(h1); free(h2); free(logits); free(probs); free(dlog);
    free(grad_w3); free(grad_b3); free(dlast);
    return loss;
}

/* ------------------------- serialization ------------------------- */
void nn_save(const nn_model *m, FILE *f) {
    int last = m->layers == 2 ? m->n_h2 : m->n_hidden;
    fwrite(&m->n_in, sizeof(int), 1, f);
    fwrite(&m->n_hidden, sizeof(int), 1, f);
    fwrite(&m->n_out, sizeof(int), 1, f);
    fwrite(&m->layers, sizeof(int), 1, f);
    fwrite(&m->n_h2, sizeof(int), 1, f);
    fwrite(m->w1, sizeof(float), (size_t)m->n_hidden * m->n_in, f);
    fwrite(m->b1, sizeof(float), (size_t)m->n_hidden, f);
    if (m->layers == 2) {
        fwrite(m->w2, sizeof(float), (size_t)m->n_h2 * m->n_hidden, f);
        fwrite(m->b2, sizeof(float), (size_t)m->n_h2, f);
    }
    fwrite(m->w3, sizeof(float), (size_t)m->n_out * last, f);
    fwrite(m->b3, sizeof(float), (size_t)m->n_out, f);
}

bool nn_load(nn_model *m, FILE *f) {
    int n_in = 0, n_hidden = 0, n_out = 0, layers = 0, n_h2 = 0;
    if (fread(&n_in, sizeof(int), 1, f) != 1) return false;
    if (fread(&n_hidden, sizeof(int), 1, f) != 1) return false;
    if (fread(&n_out, sizeof(int), 1, f) != 1) return false;
    if (fread(&layers, sizeof(int), 1, f) != 1) return false;
    if (fread(&n_h2, sizeof(int), 1, f) != 1) return false;
    nn_model *src = nn_create(n_in, n_hidden, n_out, layers, n_h2);
    if (!src) return false;
    int last = layers == 2 ? n_h2 : n_hidden;
    bool ok = fread(src->w1, sizeof(float), (size_t)n_hidden * n_in, f) == (size_t)n_hidden * n_in;
    ok = ok && fread(src->b1, sizeof(float), (size_t)n_hidden, f) == (size_t)n_hidden;
    if (layers == 2) {
        ok = ok && fread(src->w2, sizeof(float), (size_t)n_h2 * n_hidden, f) == (size_t)n_h2 * n_hidden;
        ok = ok && fread(src->b2, sizeof(float), (size_t)n_h2, f) == (size_t)n_h2;
    }
    ok = ok && fread(src->w3, sizeof(float), (size_t)n_out * last, f) == (size_t)n_out * last;
    ok = ok && fread(src->b3, sizeof(float), (size_t)n_out, f) == (size_t)n_out;
    if (!ok) { nn_free(src); return false; }
    /* move into the caller's model */
    free(m->w1); free(m->b1); free(m->w2); free(m->b2); free(m->w3); free(m->b3);
    *m = *src;
    free(src);
    m->magic = NN_MAGIC;
    return true;
}
