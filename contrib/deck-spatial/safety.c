/* Stereo-linked sample-peak protection. No lookahead, allocation or locks in run.
 * This is an overload guard, not a true-peak or loudness mastering limiter. */
#include "lv2/core/lv2.h"
#include <math.h>
#include <stdlib.h>

#define URI "urn:moonlight:deck-speaker-safety"
typedef struct {
    const float *left, *right;
    float *out_left, *out_right;
    const float *room_left, *room_right, *width_control, *distance_control;
    double gain, release, smoothing, width, distance;
    int initialized;
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
        s->smoothing = 1 - exp(-1 / (rate * .020));
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
    case 4: s->room_left = data; break;
    case 5: s->room_right = data; break;
    case 6: s->width_control = data; break;
    case 7: s->distance_control = data; break;
    }
}

static void activate(LV2_Handle handle)
{
    Safety *s = (Safety *)handle;
    s->gain = 1;
    s->initialized = 0;
}

static double control(const float *value, double fallback, double low, double high)
{
    return value && isfinite(*value) ? fmax(low, fmin(high, *value)) : fallback;
}

static void run(LV2_Handle handle, uint32_t count)
{
    Safety *s = (Safety *)handle;
    const double width = control(s->width_control, 1, .5, 1.5);
    const double distance = control(s->distance_control, 1, 0, 2);
    if (!s->initialized) {
        s->width = width;
        s->distance = distance;
        s->initialized = 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        s->width += (width - s->width) * s->smoothing;
        s->distance += (distance - s->distance) * s->smoothing;
        double l = s->left[i] + (s->room_left ? s->room_left[i] * s->distance : 0);
        double r = s->right[i] + (s->room_right ? s->room_right[i] * s->distance : 0);
        if (!isfinite(l) || !isfinite(r)) l = r = 0;
        const double mid = (l + r) * .5, side = (l - r) * .5 * s->width;
        l = mid + side;
        r = mid - side;
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
