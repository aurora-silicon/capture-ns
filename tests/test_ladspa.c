/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_ns.h"
#include <assert.h>
#include <dlfcn.h>
#include <ladspa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(library);
    const LADSPA_Descriptor *(*descriptor)(unsigned long);
    *(void **)(&descriptor) = dlsym(library, "ladspa_descriptor");
    assert(descriptor && !descriptor(2));
    char directory[] = "/tmp/capture-ns-status-XXXXXX", path[256];
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/status.json", directory);
    assert(!setenv("AURORA_NS_STATUS", path, 1));
    for (unsigned long model = 0; model < 2; model++) {
        const LADSPA_Descriptor *d = descriptor(model);
        assert(d && !d->instantiate(d, 44100));
        assert(!(d->Properties & LADSPA_PROPERTY_HARD_RT_CAPABLE));
        LADSPA_Handle instance = d->instantiate(d, NS_RATE);
        assert(instance);
        assert(!access(path, R_OK));
        float in[NS_HOP] = {0}, out[NS_HOP], late = -1, dropped = -1, latency = -1;
        d->connect_port(instance, 0, in);
        d->connect_port(instance, 1, out);
        d->connect_port(instance, 2, &late);
        d->connect_port(instance, 3, &dropped);
        d->connect_port(instance, 4, &latency);
        d->activate(instance);
        d->run(instance, 173);
        for (unsigned int i = 0; i < 173; i++) assert(out[i] == 0);
        assert(latency == (NS_DELAY_HOPS + (model ? 3u : 1u)) * NS_HOP);
        d->deactivate(instance);
        d->activate(instance);
        d->run(instance, NS_HOP);
        for (unsigned int i = 0; i < NS_HOP; i++) assert(out[i] == 0);
        /* Force overload, then compare the non-RT snapshot to LADSPA counters. */
        for (unsigned int hop = 0; hop < 100; hop++) d->run(instance, NS_HOP);
        struct timespec pause = { .tv_sec = 1, .tv_nsec = 200000000 };
        nanosleep(&pause, NULL);
        FILE *f = fopen(path, "r");
        char json[1024], expected[128];
        assert(f && fgets(json, sizeof(json), f));
        fclose(f);
        snprintf(expected, sizeof(expected), "\"late_hops\":%llu", (unsigned long long)late);
        assert(strstr(json, expected));
        snprintf(expected, sizeof(expected), "\"dropped_hops\":%llu", (unsigned long long)dropped);
        assert(strstr(json, expected));
        assert(strstr(json, "\"blocks\":102") && strstr(json, "\"engine\":\"cpu\""));
        d->cleanup(instance);
        assert(access(path, F_OK));
    }
    assert(!rmdir(directory));
    assert(!setenv("AURORA_NS_STATUS", "relative-status.json", 1));
    assert(!descriptor(0)->instantiate(descriptor(0), NS_RATE));
    assert(!setenv("AURORA_NS_STATUS", path, 1));
    assert(!descriptor(0)->instantiate(descriptor(0), NS_RATE));
    assert(!unsetenv("AURORA_NS_STATUS"));
    LADSPA_Handle plain = descriptor(0)->instantiate(descriptor(0), NS_RATE);
    assert(plain);
    descriptor(0)->cleanup(plain);
    assert(access(path, F_OK));
    dlclose(library);
    puts("LADSPA: both models, controls, activation, overload telemetry, cleanup, invalid status paths and opt-out passed");
    return 0;
}
