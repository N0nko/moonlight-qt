#include "../contrib/deck-spatial/safety.c"
#include <assert.h>
#include <float.h>
#include <stdio.h>

int main(void)
{
    const LV2_Descriptor *d = lv2_descriptor(0);
    assert(d && !lv2_descriptor(1));
    assert(!d->instantiate(d, NAN, NULL, NULL));
    LV2_Handle h = d->instantiate(d, 48000, NULL, NULL);
    assert(h);
    float l[256], r[256], a[256], b[256];
    d->connect_port(h, 0, l); d->connect_port(h, 1, r);
    d->connect_port(h, 2, a); d->connect_port(h, 3, b);
    for (int i = 0; i < 256; ++i) { l[i] = i / 512.f; r[i] = -l[i] / 2; }
    d->run(h, 256);
    for (int i = 0; i < 256; ++i) { assert(a[i] == l[i]); assert(b[i] == r[i]); }
    for (int i = 0; i < 256; ++i) { l[i] = (i + 1) * 12345.f; r[i] = -l[i] / 2; }
    l[3] = NAN; l[4] = INFINITY; l[5] = FLT_MAX;
    d->run(h, 256);
    for (int i = 0; i < 256; ++i) {
        assert(isfinite(a[i]) && fabsf(a[i]) <= .950001f);
        assert(isfinite(b[i]) && fabsf(b[i]) <= .950001f);
        if (i != 3 && i != 4 && i != 5) assert(fabsf(b[i] + a[i] / 2) < 1e-6);
    }
    // In-place processing and activation after suspend must remain safe.
    d->activate(h); d->connect_port(h, 2, l); d->connect_port(h, 3, r);
    d->run(h, 256);
    for (int i = 0; i < 256; ++i) assert(isfinite(l[i]) && fabsf(l[i]) <= .950001f);
    d->cleanup(h);

    h = d->instantiate(d, 48000, NULL, NULL);
    float wet_l[256], wet_r[256], width = 1, distance = 1;
    d->connect_port(h, 0, l); d->connect_port(h, 1, r);
    d->connect_port(h, 2, a); d->connect_port(h, 3, b);
    d->connect_port(h, 4, wet_l); d->connect_port(h, 5, wet_r);
    d->connect_port(h, 6, &width); d->connect_port(h, 7, &distance);
    for (int i = 0; i < 256; ++i) { l[i] = .1f; r[i] = -.1f; wet_l[i] = .02f; wet_r[i] = -.02f; }
    d->activate(h); d->run(h, 256);
    assert(fabsf(a[0] - .12f) < 1e-6f && fabsf(b[0] + .12f) < 1e-6f);
    width = 1.5f; distance = 2;
    d->run(h, 256);
    assert(a[0] > .12f && a[0] < .121f); // Control changes ramp, not step.
    for (int j = 0; j < 100; ++j) d->run(h, 256);
    assert(fabsf(a[255] - .21f) < 1e-6f && fabsf(b[255] + .21f) < 1e-6f);
    for (int i = 0; i < 256; ++i) { r[i] = l[i]; wet_l[i] = wet_r[i] = 0; }
    d->run(h, 256);
    assert(fabsf(a[0] - .1f) < 1e-6f && a[0] == b[0]); // Width does not move centred dialogue.
    width = INFINITY; distance = NAN;
    d->activate(h); d->run(h, 256);
    assert(isfinite(a[0]) && a[0] == b[0]);
    width = 100; distance = 100;
    for (int i = 0; i < 256; ++i) { l[i] = 5; r[i] = -5; wet_l[i] = 3; wet_r[i] = -3; }
    d->activate(h); d->run(h, 256);
    for (int i = 0; i < 256; ++i) assert(fabsf(a[i]) <= .950001f && fabsf(b[i]) <= .950001f);
    d->cleanup(h);
    puts("speaker safety: passed");
    return 0;
}
