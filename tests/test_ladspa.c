/* SPDX-License-Identifier: MIT */
#include "capture_ns.h"
#include <assert.h>
#include <dlfcn.h>
#include <ladspa.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(library);
    const LADSPA_Descriptor *(*descriptor)(unsigned long);
    *(void **)(&descriptor) = dlsym(library, "ladspa_descriptor");
    assert(descriptor && !descriptor(2));
    for (unsigned long model = 0; model < 2; model++) {
        const LADSPA_Descriptor *d = descriptor(model);
        assert(d && !d->instantiate(d, 44100));
        assert(!(d->Properties & LADSPA_PROPERTY_HARD_RT_CAPABLE));
        LADSPA_Handle instance = d->instantiate(d, NS_RATE);
        assert(instance);
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
        d->cleanup(instance);
    }
    dlclose(library);
    puts("LADSPA: both model descriptors, sample-rate rejection, controls and fresh activation passed");
    return 0;
}
