/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_ns.h"
#include <ladspa.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct instance {
    struct ns_stream *stream;
    enum ns_model model;
    float *in, *out, *late, *dropped, *latency, *errors;
    char *status_path;
    pthread_t reporter;
    int reporter_started;
    atomic_int stop_reporter;
    atomic_uint_fast64_t status_late, status_dropped, status_errors, status_blocks;
};
enum { INPUT, OUTPUT, LATE, DROPPED, LATENCY, ERRORS, PORTS };

/* Opt-in status for dedicated, single-instance clients. PipeWire 1.6's Props
 * exposes input controls, not LADSPA output notify ports. Never do file I/O,
 * allocation, formatting or waiting in run(). The reporter reads only atomic
 * snapshots, never the stream pointer (which deactivate can free). */
static int write_status(struct instance *i)
{
    size_t size = strlen(i->status_path) + 16;
    char *temporary = malloc(size);
    FILE *f;
    int fd, rc;
    if (!temporary) return -1;
    snprintf(temporary, size, "%s.tmp-XXXXXX", i->status_path);
    fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return -1; }
    f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); free(temporary); return -1; }
    rc = fprintf(f, "{\"schema\":1,\"pid\":%ld,\"engine\":\"cpu\",\"model\":\"%s\","
        "\"updated_unix\":%lld,\"late_hops\":%llu,\"dropped_hops\":%llu,"
        "\"error_hops\":%llu,\"blocks\":%llu}\n",
        (long)getpid(), i->model == NS_DF3 ? "df3" : "rnnoise", (long long)time(NULL),
        (unsigned long long)atomic_load_explicit(&i->status_late, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&i->status_dropped, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&i->status_errors, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&i->status_blocks, memory_order_relaxed));
    if (fclose(f) || rc < 0) rc = -1;
    else rc = rename(temporary, i->status_path);
    if (rc) unlink(temporary);
    free(temporary);
    return rc;
}

static void *report_status(void *opaque)
{
    struct instance *i = opaque;
    unsigned int tick = 0;
    while (!atomic_load_explicit(&i->stop_reporter, memory_order_acquire)) {
        struct timespec pause = { .tv_nsec = 100000000 };
        nanosleep(&pause, NULL);
        if (++tick == 10) {
            (void)write_status(i);
            tick = 0;
        }
    }
    return NULL;
}

static int start_reporter(struct instance *i)
{
    const char *path = getenv("AURORA_NS_STATUS");
    atomic_init(&i->stop_reporter, 0);
    atomic_init(&i->status_late, 0);
    atomic_init(&i->status_dropped, 0);
    atomic_init(&i->status_errors, 0);
    atomic_init(&i->status_blocks, 0);
    if (!path || !*path) return 0;
    if (*path != '/' || !(i->status_path = strdup(path))) return -1;
    if (write_status(i) || pthread_create(&i->reporter, NULL, report_status, i)) {
        unlink(i->status_path);
        free(i->status_path);
        i->status_path = NULL;
        return -1;
    }
    i->reporter_started = 1;
    return 0;
}

static void stop_reporter(struct instance *i)
{
    if (i->reporter_started) {
        atomic_store_explicit(&i->stop_reporter, 1, memory_order_release);
        pthread_join(i->reporter, NULL);
    }
    if (i->status_path) unlink(i->status_path);
    free(i->status_path);
}
static float latency_samples(const struct instance *i)
{
    return NS_DELAY_HOPS * NS_HOP + (i->model == NS_DF3 ? 3u : 1u) * NS_HOP;
}
static LADSPA_Handle instantiate(const LADSPA_Descriptor *d, unsigned long rate)
{
    struct instance *i;
    if (rate != NS_RATE) return NULL;
    i = calloc(1, sizeof(*i));
    if (!i) return NULL;
    i->model = strcmp(d->Label, "aurora_df3") == 0 ? NS_DF3 : NS_RNNOISE;
    i->stream = ns_stream_new(i->model);
    if (!i->stream) { free(i); return NULL; }
    if (start_reporter(i)) {
        fprintf(stderr, "capture-ns: cannot initialize requested status file\n");
        ns_stream_free(i->stream);
        free(i);
        return NULL;
    }
    return i;
}
static void connect_port(LADSPA_Handle h, unsigned long port, LADSPA_Data *data)
{
    struct instance *i = h;
    switch (port) {
    case INPUT: i->in = data; break;
    case OUTPUT: i->out = data; break;
    case LATE: i->late = data; break;
    case DROPPED: i->dropped = data; break;
    case ERRORS: i->errors = data; break;
    case LATENCY:
        i->latency = data;
        if (data) *data = latency_samples(i);
        break;
    }
}
static void run(LADSPA_Handle h, unsigned long samples)
{
    struct instance *i = h;
    struct ns_stream_stats stats;
    if (!i->out) return;
    if (!i->stream) {
        memset(i->out, 0, samples * sizeof(float));
        return;
    }
    ns_stream_process(i->stream, i->in, i->out, samples);
    ns_stream_stats(i->stream, &stats);
    if (i->reporter_started) {
        atomic_store_explicit(&i->status_late, stats.late, memory_order_relaxed);
        atomic_store_explicit(&i->status_dropped, stats.dropped, memory_order_relaxed);
        atomic_store_explicit(&i->status_errors, stats.errors, memory_order_relaxed);
        atomic_fetch_add_explicit(&i->status_blocks, 1, memory_order_relaxed);
    }
    if (i->late) *i->late = stats.late;
    if (i->dropped) *i->dropped = stats.dropped;
    if (i->errors) *i->errors = stats.errors;
    if (i->latency) *i->latency = latency_samples(i);
}
static void activate(LADSPA_Handle h)
{
    struct instance *i = h;
    if (!i->stream) i->stream = ns_stream_new(i->model);
}
static void deactivate(LADSPA_Handle h)
{
    struct instance *i = h;
    ns_stream_free(i->stream);
    i->stream = NULL;
}
static void cleanup(LADSPA_Handle h)
{
    struct instance *i = h;
    stop_reporter(i);
    ns_stream_free(i->stream);
    free(i);
}
static const LADSPA_PortDescriptor ports[PORTS] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO, LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL
};
/* PipeWire recognizes this exact notify-port name as process latency. */
static const char *names[PORTS] = { "Input", "Output", "Late Hops", "Dropped Hops", "latency", "Error Hops" };
static const LADSPA_PortRangeHint hints[PORTS] = {{0}, {0}, {0}, {0},
    { .HintDescriptor = LADSPA_HINT_INTEGER | LADSPA_HINT_BOUNDED_BELOW, .LowerBound = 0 }, {0}};
/* Experimental IDs, not a LADSPA registry allocation. */
static const LADSPA_Descriptor descriptors[] = {
    { .UniqueID = 7312451, .Label = "aurora_rnnoise", .Name = "Aurora capture RNNoise",
      .Maker = "Aurora Silicon", .Copyright = "MIT", .PortCount = PORTS,
      .PortDescriptors = ports, .PortNames = names, .PortRangeHints = hints,
      .instantiate = instantiate, .connect_port = connect_port, .activate = activate,
      .deactivate = deactivate, .run = run, .cleanup = cleanup },
    { .UniqueID = 7312452, .Label = "aurora_df3", .Name = "Aurora capture DeepFilterNet3",
      .Maker = "Aurora Silicon", .Copyright = "MIT", .PortCount = PORTS,
      .PortDescriptors = ports, .PortNames = names, .PortRangeHints = hints,
      .instantiate = instantiate, .connect_port = connect_port, .activate = activate,
      .deactivate = deactivate, .run = run, .cleanup = cleanup }
};
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    return index < sizeof(descriptors) / sizeof(descriptors[0]) ? &descriptors[index] : NULL;
}
