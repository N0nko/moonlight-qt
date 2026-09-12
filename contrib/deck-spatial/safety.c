/* Stereo-linked sample-peak protection. No lookahead, allocation or locks in run.
 * This is an overload guard, not a true-peak or loudness mastering limiter. */
#include "lv2/core/lv2.h"
#include <math.h>
#include <stdlib.h>

#define URI "urn:moonlight:deck-speaker-safety"
typedef struct {
    const float *left, *right;
    float *out_left, *out_right;
    double gain, release;
} Safety;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate,
                             const char *path, const LV2_Feature *const *features)
{
    (void)d; (void)path; (void)features;
    if (!isfinite(rate) || rate < 8000) return NULL;
    Safety *s = (Safety *)calloc(1, sizeof(*s));
    if (s) {
        s->gain = 1;
        s->release = 1 - exp(-1 / (rate * .100));
    }
    return s;
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data)
{
    Safety *s = (Safety *)handle;
    switch (port) {
    case 0: s->left = data; break;
    case 1: s->right = data; break;
    case 2: s->out_left = data; break;
    case 3: s->out_right = data; break;
    }
}

static void activate(LV2_Handle handle) { ((Safety *)handle)->gain = 1; }

static void run(LV2_Handle handle, uint32_t count)
{
    Safety *s = (Safety *)handle;
    for (uint32_t i = 0; i < count; ++i) {
        double l = s->left[i], r = s->right[i];
        if (!isfinite(l) || !isfinite(r)) l = r = 0;
        double peak = fmax(fabs(l), fabs(r));
        double ceiling = peak > .95 ? .95 / peak : 1;
        s->gain = fmin(ceiling, s->gain + (1 - s->gain) * s->release);
        s->out_left[i] = (float)(l * s->gain);
        s->out_right[i] = (float)(r * s->gain);
    }
}

static void cleanup(LV2_Handle handle) { free(handle); }

static const LV2_Descriptor descriptor = {
    URI, instantiate, connect_port, activate, run, NULL, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
