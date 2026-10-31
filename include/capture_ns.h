/* SPDX-License-Identifier: MIT */
#ifndef CAPTURE_NS_H
#define CAPTURE_NS_H
#include <stddef.h>
#include <stdint.h>
#define NS_HOP 480u
#define NS_RATE 48000u
#define NS_DELAY_HOPS 2u

enum ns_model { NS_RNNOISE, NS_DF3 };
struct ns_engine;
struct ns_stream;
struct ns_stats {
    uint64_t calls, cpu_calls, native_calls, failed_calls;
    uint64_t total_ns, max_ns, cpu_ns, last_ns;
};
struct ns_stream_stats { uint64_t dropped, late, errors; };

/* Non-RT lifecycle. Each instance owns independent DSP/recurrent state. */
struct ns_engine *ns_engine_new(enum ns_model model);
void ns_engine_free(struct ns_engine *engine);
int ns_engine_process(struct ns_engine *engine, const float in[NS_HOP], float out[NS_HOP]);
void ns_engine_stats(const struct ns_engine *engine, struct ns_stats *stats);

/* Inference runs on a worker. The audio callback never waits for inference.
 * Missing deadlines produce silence, never an unfiltered microphone signal.
 * Lifecycle calls must not race ns_stream_process. */
struct ns_stream *ns_stream_new(enum ns_model model);
void ns_stream_free(struct ns_stream *stream);
void ns_stream_process(struct ns_stream *stream, const float *in, float *out, size_t samples);
void ns_stream_stats(const struct ns_stream *stream, struct ns_stream_stats *stats);
#endif
