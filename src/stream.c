/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_ns.h"
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE 8u
struct frame { uint64_t sequence; float data[NS_HOP]; };
struct queue { _Atomic unsigned int read, write; struct frame frames[QUEUE]; };
struct ns_stream {
    enum ns_model model;
    struct ns_engine *engine;
    struct queue input, output;
    pthread_t worker;
    sem_t wake;
    _Atomic unsigned int stop;
    _Atomic uint64_t dropped, late, errors;
    uint64_t sequence;
    size_t offset;
    float in[NS_HOP], out[NS_HOP];
};

/* One producer and one consumer. Release publishes an entire frame. */
static int put(struct queue *q, const struct frame *f)
{
    unsigned int w = atomic_load_explicit(&q->write, memory_order_relaxed);
    unsigned int r = atomic_load_explicit(&q->read, memory_order_acquire);
    if (w - r == QUEUE) return 0;
    q->frames[w % QUEUE] = *f;
    atomic_store_explicit(&q->write, w + 1, memory_order_release);
    return 1;
}

static int peek(struct queue *q, struct frame *f)
{
    unsigned int r = atomic_load_explicit(&q->read, memory_order_relaxed);
    unsigned int w = atomic_load_explicit(&q->write, memory_order_acquire);
    if (r == w) return 0;
    *f = q->frames[r % QUEUE];
    return 1;
}

static void pop(struct queue *q)
{
    atomic_fetch_add_explicit(&q->read, 1, memory_order_release);
}

static void *work(void *context)
{
    struct ns_stream *s = context;
    struct frame in, out;
    uint64_t next = 0;
    while (!atomic_load_explicit(&s->stop, memory_order_acquire)) {
        while (sem_wait(&s->wake) && errno == EINTR) {}
        if (atomic_load_explicit(&s->stop, memory_order_acquire)) break;
        if (!peek(&s->input, &in)) continue;
        pop(&s->input);
        if (in.sequence != next) {
            /* A lost hop invalidates recurrent/STFT history. Reset on the
             * worker, never in the graph callback. */
            ns_engine_free(s->engine);
            s->engine = ns_engine_new(s->model);
        }
        next = in.sequence + 1;
        out.sequence = in.sequence;
        if (!s->engine || ns_engine_process(s->engine, in.data, out.data)) {
            memset(out.data, 0, sizeof(out.data));
            atomic_fetch_add_explicit(&s->errors, 1, memory_order_relaxed);
            next = UINT64_MAX;
        }
        if (!put(&s->output, &out))
            atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed);
    }
    return NULL;
}

struct ns_stream *ns_stream_new(enum ns_model model)
{
    struct ns_stream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->model = model;
    atomic_init(&s->stop, 0);
    atomic_init(&s->dropped, 0);
    atomic_init(&s->late, 0);
    atomic_init(&s->errors, 0);
    atomic_init(&s->input.read, 0);
    atomic_init(&s->input.write, 0);
    atomic_init(&s->output.read, 0);
    atomic_init(&s->output.write, 0);
    /* These operations must never hide an internal lock in the callback. */
    if (!atomic_is_lock_free(&s->dropped) || !atomic_is_lock_free(&s->input.write)) goto fail;
    if (!(s->engine = ns_engine_new(model))) goto fail;
    if (sem_init(&s->wake, 0, 0)) goto fail;
    if (pthread_create(&s->worker, NULL, work, s)) {
        sem_destroy(&s->wake);
        goto fail;
    }
    return s;
fail:
    ns_engine_free(s->engine);
    free(s);
    return NULL;
}

void ns_stream_free(struct ns_stream *s)
{
    if (!s) return;
    atomic_store_explicit(&s->stop, 1, memory_order_release);
    sem_post(&s->wake);
    pthread_join(s->worker, NULL);
    sem_destroy(&s->wake);
    ns_engine_free(s->engine);
    free(s);
}

void ns_stream_process(struct ns_stream *s, const float *in, float *out, size_t n)
{
    struct frame f;
    for (size_t i = 0; i < n; i++) {
        if (s->offset == 0) {
            memset(s->out, 0, sizeof(s->out));
            if (s->sequence >= NS_DELAY_HOPS) {
                uint64_t wanted = s->sequence - NS_DELAY_HOPS;
                int found = 0;
                while (peek(&s->output, &f)) {
                    if (f.sequence > wanted) break;
                    pop(&s->output);
                    if (f.sequence == wanted) {
                        memcpy(s->out, f.data, sizeof(s->out));
                        found = 1;
                    }
                }
                if (!found) atomic_fetch_add_explicit(&s->late, 1, memory_order_relaxed);
            }
        }
        /* Read before writing so in-place hosts work too. */
        s->in[s->offset] = in ? in[i] : 0.0f;
        out[i] = s->out[s->offset];
        if (++s->offset == NS_HOP) {
            f.sequence = s->sequence++;
            memcpy(f.data, s->in, sizeof(s->in));
            if (put(&s->input, &f)) sem_post(&s->wake);
            else atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed);
            s->offset = 0;
        }
    }
}

void ns_stream_stats(const struct ns_stream *s, struct ns_stream_stats *stats)
{
    stats->dropped = atomic_load_explicit(&s->dropped, memory_order_relaxed);
    stats->late = atomic_load_explicit(&s->late, memory_order_relaxed);
    stats->errors = atomic_load_explicit(&s->errors, memory_order_relaxed);
}
