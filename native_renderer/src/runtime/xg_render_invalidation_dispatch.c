#include "xg_render_invalidation_dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { XG_RENDER_INVALIDATION_MODULE_CAPACITY = 32u };

static XgRenderInvalidationModule
    modules[XG_RENDER_INVALIDATION_MODULE_CAPACITY];
static uint32_t module_count;
static uint64_t timing_calls[32],timing_ns[32];
static const char *timing_path;
static uint64_t timing_now(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
#else
    return 0;
#endif
}
static void timing_dump(void) {
    if(!timing_path)return;
    FILE *f=fopen(timing_path,"w");if(!f)return;
    fputs("handler,calls,ns\n",f);
    for(uint32_t i=0;i<module_count;++i)fprintf(f,"%llx,%llu,%llu\n",
        (unsigned long long)(uintptr_t)modules[i].handle,
        (unsigned long long)timing_calls[i],(unsigned long long)timing_ns[i]);
    fclose(f);
}

void xg_render_invalidation_clear_modules(void) {
    module_count = 0u;
    static int initialized;
    if(!initialized) {
        timing_path=getenv("PSX_INVALIDATION_TIMING_OUT");
        if(timing_path)atexit(timing_dump);
        initialized=1;
    }
}

bool xg_render_invalidation_register_module(
        const XgRenderInvalidationModule *module) {
    if (module == NULL || module->handle == NULL ||
        module_count >= XG_RENDER_INVALIDATION_MODULE_CAPACITY)
        return false;
    modules[module_count++] = *module;
    return true;
}

void xg_render_invalidation_dispatch(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    if (event == NULL || services == NULL) return;
    for (uint32_t index = 0u; index < module_count; ++index) {
        const uint64_t start=timing_path?timing_now():0;
        modules[index].handle(event, services);
        if(timing_path){timing_calls[index]++;timing_ns[index]+=timing_now()-start;}
    }
}
