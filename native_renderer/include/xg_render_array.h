#ifndef XG_RENDER_ARRAY_H
#define XG_RENDER_ARRAY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Grow storage for records whose complete value is assigned before publication.
 * Failure preserves both contents and capacity. */
static inline void *xg_render_array_reserve_uninitialized(void *data, size_t element_size,
        uint32_t *capacity, uint32_t required, uint32_t maximum) {
    if (required <= *capacity) return data;
    if (!element_size || required > maximum) return NULL;
    uint32_t next = *capacity ? *capacity : (maximum < 16u ? maximum : 16u);
    while (next < required)
        next = next > maximum / 2u ? maximum : next * 2u;
    if ((size_t)next > SIZE_MAX / element_size) return NULL;
    void *grown = realloc(data, (size_t)next * element_size);
    if (!grown) return NULL;
    *capacity = next;
    return grown;
}

/* General arrays retain zero initialization of all newly allocated slots. */
static inline void *xg_render_array_reserve(void *data, size_t element_size,
        uint32_t *capacity, uint32_t required, uint32_t maximum) {
    const uint32_t previous=*capacity;
    void *grown=xg_render_array_reserve_uninitialized(data,element_size,capacity,required,maximum);
    if (grown && *capacity>previous)
        memset((unsigned char *)grown+(size_t)previous*element_size,0,
            (size_t)(*capacity-previous)*element_size);
    return grown;
}

/* Append-only streaming allocations retain old backing stores until readers of
 * published prefixes finish. Publish the returned pointer under the caller's
 * synchronization; growing alone does not modify any previously published byte. */
typedef struct XgRenderRetiredBuffer {
    void *data;
    struct XgRenderRetiredBuffer *next;
} XgRenderRetiredBuffer;

static inline void *xg_render_append_reserve(void *data, size_t element_size,
        uint32_t *capacity, uint32_t used, uint32_t required, uint32_t maximum,
        XgRenderRetiredBuffer **retired) {
    if (required <= *capacity) return data;
    if (!element_size || required > maximum || used > *capacity) return NULL;
    uint32_t next = *capacity ? *capacity : (maximum < 16u ? maximum : 16u);
    while (next < required) next = next > maximum / 2u ? maximum : next * 2u;
    if ((size_t)next > SIZE_MAX / element_size) return NULL;
    XgRenderRetiredBuffer *old = data ? malloc(sizeof(*old)) : NULL;
    if (data && !old) return NULL;
    void *grown = malloc((size_t)next * element_size);
    if (!grown) { free(old); return NULL; }
    if (used) memcpy(grown, data, (size_t)used * element_size);
    if (old) {
        *old = (XgRenderRetiredBuffer){data, *retired};
        *retired = old;
    }
    *capacity = next;
    return grown;
}

static inline void xg_render_retired_buffers_free(XgRenderRetiredBuffer **retired) {
    while (*retired) {
        XgRenderRetiredBuffer *old = *retired;
        *retired = old->next;
        free(old->data);
        free(old);
    }
}

#endif
