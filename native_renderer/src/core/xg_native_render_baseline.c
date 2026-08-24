#include "xg_native_render_baseline.h"

#include <limits.h>
#include <string.h>

#define XG_FNV_OFFSET UINT64_C(1469598103934665603)
#define XG_FNV_PRIME UINT64_C(1099511628211)
#define XG_GUEST_RAM_BYTES (2u * 1024u * 1024u)
#define XG_CAMERA_EYE_PHYS UINT32_C(0x000af880)
#define XG_CAMERA_AT_PHYS UINT32_C(0x000af890)
#define XG_CAMERA_MATRIX_PHYS UINT32_C(0x000af990)
#define XG_PROJECTION_MATRIX_PHYS UINT32_C(0x000afa64)
#define XG_ACTOR_COUNT_PHYS UINT32_C(0x000afb0c)
#define XG_ACTOR_POINTER_PHYS UINT32_C(0x000afb10)
#define XG_ACTOR_STRIDE UINT32_C(0x5c)

extern const uint8_t xg_render_game_identity[32];
extern const uint8_t xg_render_manifest_identity[32];

static XgNativeRenderBaselineResult failure(
        XgNativeRenderBaselineReason reason) {
    XgNativeRenderBaselineResult result = { false, reason, 0u };
    return result;
}

static XgNativeRenderBaselineResult success(uint64_t digest) {
    XgNativeRenderBaselineResult result = {
        true,
        XG_NATIVE_RENDER_BASELINE_OK,
        digest == 0u ? UINT64_C(1) : digest,
    };
    return result;
}

static uint64_t hash_bytes(uint64_t digest, const uint8_t *bytes, size_t size) {
    size_t index;
    for (index = 0; index < size; ++index) {
        digest ^= bytes[index];
        digest *= XG_FNV_PRIME;
    }
    return digest;
}

static uint32_t read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static int guest_pointer_phys(uint32_t pointer, uint32_t *out_phys) {
    const uint32_t segment = pointer & UINT32_C(0xe0000000);
    if (segment != 0u && segment != UINT32_C(0x80000000) &&
        segment != UINT32_C(0xa0000000)) return 0;
    *out_phys = pointer & UINT32_C(0x1fffffff);
    return *out_phys < XG_GUEST_RAM_BYTES;
}

XgNativeRenderBaselineResult xg_native_render_baseline_configure(
        const PsxGameIdentity *runtime_identity,
        uint32_t max_vblanks,
        NativeRenderBaselineConfig *out_config) {
    uint64_t digest = XG_FNV_OFFSET;
    if (!out_config) return failure(XG_NATIVE_RENDER_BASELINE_INVALID_ARGUMENT);
    *out_config = (NativeRenderBaselineConfig){ 0u };
    if (!runtime_identity ||
        memcmp(runtime_identity->game_sha256,
               xg_render_game_identity,
               sizeof(runtime_identity->game_sha256)) != 0 ||
        memcmp(runtime_identity->manifest_sha256,
               xg_render_manifest_identity,
               sizeof(runtime_identity->manifest_sha256)) != 0)
        return failure(XG_NATIVE_RENDER_BASELINE_INVALID_IDENTITY);
    if (max_vblanks == 0u ||
        max_vblanks > NATIVE_RENDER_BASELINE_VBLANK_CAPACITY)
        return failure(XG_NATIVE_RENDER_BASELINE_INVALID_VBLANK_BOUND);
    digest = hash_bytes(digest, runtime_identity->game_sha256,
                        sizeof(runtime_identity->game_sha256));
    digest = hash_bytes(digest, runtime_identity->manifest_sha256,
                        sizeof(runtime_identity->manifest_sha256));
    if (digest == 0u) digest = UINT64_C(1);
    out_config->max_vblanks = max_vblanks;
    out_config->game_digest = digest;
    return success(digest);
}

XgNativeRenderBaselineResult xg_native_render_baseline_sample(
        const uint8_t *guest_ram,
        size_t guest_ram_size) {
    uint64_t digest = XG_FNV_OFFSET;
    uint32_t actor_count;
    uint32_t actor_pointer;
    uint32_t actor_phys;
    size_t actor_bytes;
    if (!guest_ram || guest_ram_size != XG_GUEST_RAM_BYTES)
        return failure(XG_NATIVE_RENDER_BASELINE_INVALID_ARGUMENT);

    actor_count = read_u32_le(guest_ram + XG_ACTOR_COUNT_PHYS);
    actor_pointer = read_u32_le(guest_ram + XG_ACTOR_POINTER_PHYS);
    if (actor_count > XG_NATIVE_RENDER_BASELINE_ACTOR_COUNT_CAP)
        return failure(XG_NATIVE_RENDER_BASELINE_INVALID_ACTOR_COUNT);
    if (!guest_pointer_phys(actor_pointer, &actor_phys))
        return failure(XG_NATIVE_RENDER_BASELINE_INVALID_ACTOR_POINTER);
    if ((size_t)actor_count > SIZE_MAX / XG_ACTOR_STRIDE)
        return failure(XG_NATIVE_RENDER_BASELINE_ACTOR_RANGE_OVERFLOW);
    actor_bytes = (size_t)actor_count * XG_ACTOR_STRIDE;
    if (actor_bytes > guest_ram_size - actor_phys)
        return failure(XG_NATIVE_RENDER_BASELINE_ACTOR_RANGE_OVERFLOW);

    digest = hash_bytes(digest, guest_ram + XG_CAMERA_EYE_PHYS, 8u);
    digest = hash_bytes(digest, guest_ram + XG_CAMERA_AT_PHYS, 8u);
    digest = hash_bytes(digest, guest_ram + XG_CAMERA_MATRIX_PHYS, 32u);
    digest = hash_bytes(digest, guest_ram + XG_PROJECTION_MATRIX_PHYS, 32u);
    digest = hash_bytes(digest, guest_ram + XG_ACTOR_COUNT_PHYS, 4u);
    digest = hash_bytes(digest, guest_ram + actor_phys, actor_bytes);
    return success(digest);
}
