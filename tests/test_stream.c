/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_ns.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct ns_engine { int unused; };
static _Atomic unsigned int completed, slow;
static void pause_ms(unsigned int ms)
{
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000u };
    nanosleep(&t, NULL);
}
struct ns_engine *ns_engine_new(enum ns_model model)
{
    (void)model;
    return calloc(1, sizeof(struct ns_engine));
}
void ns_engine_free(struct ns_engine *e) { free(e); }
int ns_engine_process(struct ns_engine *e, const float in[NS_HOP], float out[NS_HOP])
{
    (void)e;
    while (atomic_load(&slow)) pause_ms(1);
    for (unsigned int i = 0; i < NS_HOP; i++) out[i] = in[i] * 0.5f;
    atomic_fetch_add(&completed, 1);
    return 0;
}
static void wait_for(unsigned int n)
{
    for (unsigned int j = 0; j < 1000 && atomic_load(&completed) < n; j++) pause_ms(1);
    assert(atomic_load(&completed) >= n);
    /* The worker publishes immediately after the fake engine returns. */
    pause_ms(2);
}
static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
int main(void)
{
    struct ns_stream *s = ns_stream_new(NS_RNNOISE);
    struct ns_stream_stats stats;
    float buffer[NS_HOP];
    const size_t quanta[] = {17, 127, 1, 335};
    assert(s);
    for (unsigned int hop = 0; hop < 12; hop++) {
        for (unsigned int i = 0; i < NS_HOP; i++) buffer[i] = (float)(hop + 1) / 100;
        size_t offset = 0;
        for (unsigned int q = 0; q < 4; q++) {
            ns_stream_process(s, buffer + offset, buffer + offset, quanta[q]);
            offset += quanta[q];
        }
        for (unsigned int i = 0; i < NS_HOP; i++) {
            float expected = hop < NS_DELAY_HOPS ? 0 : (float)(hop - NS_DELAY_HOPS + 1) / 200;
            assert(fabsf(buffer[i] - expected) < 1e-7f);
        }
        wait_for(hop + 1);
    }
    ns_stream_stats(s, &stats);
    assert(!stats.late && !stats.dropped && !stats.errors);
    ns_stream_free(s);
    s = ns_stream_new(NS_RNNOISE);
    assert(s);
    unsigned int completed_before = atomic_load(&completed);
    const size_t ragged[] = {17, 127, 319, 1, 16, 251, 389};
    size_t total = 0;
    float separate[NS_HOP];
    for (unsigned int block = 0; block < 42; block++) {
        size_t n = ragged[block % 7];
        for (size_t i = 0; i < n; i++) buffer[i] = (float)((total+i) % 1000) / 10000;
        ns_stream_process(s, buffer, separate, n);
        for (size_t i = 0; i < n; i++) {
            size_t position = total + i;
            float expected = position < NS_DELAY_HOPS * NS_HOP ? 0 :
                (float)((position - NS_DELAY_HOPS * NS_HOP) % 1000) / 20000;
            assert(fabsf(separate[i] - expected) < 1e-7f);
        }
        total += n;
        wait_for(completed_before + total / NS_HOP);
    }
    ns_stream_stats(s, &stats);
    assert(!stats.late && !stats.dropped && !stats.errors);
    ns_stream_free(s);
    s = ns_stream_new(NS_RNNOISE);
    assert(s);
    atomic_store(&slow, 1);
    uint64_t start = now_ns();
    for (unsigned int hop = 0; hop < 100; hop++) {
        for (unsigned int i = 0; i < NS_HOP; i++) buffer[i] = 0.1f;
        ns_stream_process(s, buffer, buffer, NS_HOP);
        for (unsigned int i = 0; i < NS_HOP; i++) assert(buffer[i] == 0.0f);
    }
    assert(now_ns() - start < 50000000u);
    ns_stream_stats(s, &stats);
    assert(stats.dropped && stats.late);
    atomic_store(&slow, 0);
    ns_stream_free(s);
    puts("stream: irregular/in-place quanta, exact delay, overload silence and bounded callback passed");
    return 0;
}
