/* SPDX-License-Identifier: MIT */
#include "capture_ns.h"
#include <ladspa.h>
#include <stdlib.h>
#include <string.h>

struct instance { struct ns_stream *stream; enum ns_model model; float *in, *out, *late, *dropped, *latency, *errors; };
enum { INPUT, OUTPUT, LATE, DROPPED, LATENCY, ERRORS, PORTS };
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
