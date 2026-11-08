/* nn.h — tiny dense neural net used by both engines (plain C, no PyTorch).
 * Layout: model = linear(1) -> relu -> linear(2) -> (softmax out).
 * Training: Adam with optional momentum, mini-batch SGD. */
#ifndef OSAI_NN_H
#define OSAI_NN_H

#include "osai_common.h"

typedef struct nn_model {
    int     n_in, n_hidden, n_out;
    int     layers;              /* 1 or 2 hidden layers */
    int     n_h2;
    float  *w1, *b1;             /* n_hidden x n_in, n_hidden */
    float  *w2, *b2;             /* n_h2 x n_hidden, n_h2 (0 when layers==1) */
    float  *w3, *b3;             /* n_out x n_h2 (or n_hidden), n_out */
    uint32_t magic;
} nn_model;

nn_model *nn_create(int n_in, int n_hidden, int n_out, int layers, int n_h2);
void      nn_free(nn_model *m);
void      nn_init_random(nn_model *m, uint32_t seed);
double    nn_forward(const nn_model *m, const float *x, float *out);
/* Trains one sample with cross-entropy + Adam. `adam_mv` is a caller-owned
 * buffer of 2*nn_param_count(m) floats. Returns the loss. */
double    nn_train_sample(nn_model *m, const float *x, int label, float lr,
                          float *adam_mv, int *adam_t, float clip);
int       nn_param_count(const nn_model *m);
void      nn_save(const nn_model *m, FILE *f);
bool      nn_load(nn_model *m, FILE *f);

/* softmax + argmax helpers */
void nn_softmax(float *v, int n);
int  nn_argmax(const float *v, int n);

#endif
