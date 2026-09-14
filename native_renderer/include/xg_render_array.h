#ifndef XG_RENDER_ARRAY_H
#define XG_RENDER_ARRAY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Grow host storage without changing contents or capacity on failure. New
 * slots are zeroed; maximum describes an address/index domain, not a budget. */
static inline void *xg_render_array_reserve(void *data, size_t element_size,
        uint32_t *capacity, uint32_t required, uint32_t maximum) {
    if (required <= *capacity) return data;
    if (!element_size || required > maximum) return NULL;
    uint32_t next = *capacity ? *capacity : (maximum < 16u ? maximum : 16u);
    while (next < required)
        next = next > maximum / 2u ? maximum : next * 2u;
    if ((size_t)next > SIZE_MAX / element_size) return NULL;
    void *grown = realloc(data, (size_t)next * element_size);
    if (!grown) return NULL;
    memset((unsigned char *)grown + (size_t)*capacity * element_size, 0,
        (size_t)(next - *capacity) * element_size);
    *capacity = next;
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
