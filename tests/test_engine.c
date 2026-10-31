/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_ns.h"
#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    struct ns_engine *a = ns_engine_new(NS_RNNOISE), *b = ns_engine_new(NS_RNNOISE);
    struct ns_stats stats;
    void *library = dlopen(getenv("AURORA_RNNOISE_LIBRARY"), RTLD_NOW | RTLD_LOCAL);
    void *(*create)(void *);
    float (*process)(void *, float *, const float *);
    void (*destroy)(void *);
    assert(library);
    *(void **)(&create) = dlsym(library, "rnnoise_create");
    *(void **)(&process) = dlsym(library, "rnnoise_process_frame");
    *(void **)(&destroy) = dlsym(library, "rnnoise_destroy");
    assert(create && process && destroy);
    void *original = create(NULL);
    assert(original);
    float in[NS_HOP], out[NS_HOP], reference[NS_HOP];
    assert(a && b);
    for (unsigned int frame = 0; frame < 30; frame++) {
        for (unsigned int i = 0; i < NS_HOP; i++)
            in[i] = 0.1f * sinf((frame * NS_HOP + i) * 0.056f);
        assert(!ns_engine_process(a, in, out));
        assert(!ns_engine_process(b, in, reference));
        assert(!memcmp(out, reference, sizeof(out)));
        float pcm[NS_HOP];
        for (unsigned int i = 0; i < NS_HOP; i++) pcm[i] = in[i] * 32768.0f;
        process(original, reference, pcm);
        for (unsigned int i = 0; i < NS_HOP; i++) reference[i] /= 32768.0f;
        assert(!memcmp(out, reference, sizeof(out)));
        for (unsigned int i = 0; i < NS_HOP; i++) assert(isfinite(out[i]));
    }
    ns_engine_stats(a, &stats);
    assert(stats.calls == 30 && stats.cpu_calls == 30 && stats.native_calls == 0);
    assert(stats.total_ns && stats.cpu_ns && stats.last_ns);
    in[15] = NAN;
    assert(ns_engine_process(a, in, out) < 0);
    for (unsigned int i = 0; i < NS_HOP; i++) assert(out[i] == 0.0f);
    ns_engine_free(a);
    ns_engine_free(b);
    destroy(original);
    dlclose(library);
    assert(!ns_engine_new((enum ns_model)99));
    unsetenv("AURORA_RNNOISE_MODEL");
    assert(!ns_engine_new(NS_RNNOISE));
    setenv("AURORA_DF_MODEL", "/dev/null", 1);
    assert(!ns_engine_new(NS_DF3));
    puts("engine: original-weight bit parity, isolated states, CPU dispatch, model rejection and invalid input passed");
    return 0;
}
