#ifndef XG_MODEL_PRIMITIVE_LAYOUT_H
#define XG_MODEL_PRIMITIVE_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

enum { XG_MODEL_PRIMITIVE_FAMILY_COUNT = 17 };

typedef struct XgModelPrimitiveLayout {
    uint8_t packet_size;
    uint8_t attribute_size;
    uint8_t vertex_count;
} XgModelPrimitiveLayout;

/* Resident model dispatch LUT at 8004fe50. Every row consumes eight bytes of
 * topology; bit 3 selects quads. Row 16 is the mapped-texture triangle lane. */
static inline bool xg_model_primitive_layout(uint32_t family,
                                             XgModelPrimitiveLayout *out) {
    static const uint8_t packet_sizes[XG_MODEL_PRIMITIVE_FAMILY_COUNT] = {
        20, 32, 28, 40, 20, 32, 28, 40, 24, 40, 36, 52, 24, 40, 36, 52, 32,
    };
    static const uint8_t attribute_sizes[XG_MODEL_PRIMITIVE_FAMILY_COUNT] = {
        4, 8, 4, 8, 4, 8, 4, 8, 4, 12, 4, 12, 4, 12, 4, 12, 4,
    };
    if (family >= XG_MODEL_PRIMITIVE_FAMILY_COUNT || out == 0) return false;
    *out = (XgModelPrimitiveLayout){packet_sizes[family], attribute_sizes[family],
        (uint8_t)((family & 8u) ? 4u : 3u)};
    return true;
}

#endif
