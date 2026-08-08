#include "xg_native_render_baseline.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
#define SEED_CONFIG(config) ((config) = (NativeRenderBaselineConfig){ UINT32_MAX, UINT32_MAX, UINT64_MAX })
#define CHECK_ZERO_CONFIG(config) do { \
    CHECK((config).authenticated_producer_address == 0u); \
    CHECK((config).max_vblanks == 0u); \
    CHECK((config).game_digest == 0u); \
} while (0)

const uint8_t xg_render_game_identity[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
const uint8_t xg_render_manifest_identity[32] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
};

static void write_u32_le(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static uint64_t expected_identity_digest(const PsxGameIdentity *identity) {
    const uint8_t *bytes = (const uint8_t *)identity;
    uint64_t digest = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0; index < sizeof(*identity); ++index) {
        digest ^= bytes[index];
        digest *= UINT64_C(1099511628211);
    }
    return digest == 0u ? UINT64_C(1) : digest;
}

int main(void) {
    const size_t ram_size = 2u * 1024u * 1024u;
    const uint32_t actor_phys = 0x00100000u;
    PsxGameIdentity identity;
    NativeRenderBaselineConfig config;
    XgNativeRenderBaselineResult result;
    XgNativeRenderBaselineResult repeated;
    uint8_t *ram = (uint8_t *)calloc(ram_size, 1u);
    uint64_t original_digest;
    size_t index;
    CHECK(ram != NULL);

    memcpy(identity.game_sha256, xg_render_game_identity, 32u);
    memcpy(identity.manifest_sha256, xg_render_manifest_identity, 32u);
    result = xg_native_render_baseline_configure(&identity, 3508u, &config);
    CHECK(result.success);
    CHECK(result.reason == XG_NATIVE_RENDER_BASELINE_OK);
    CHECK(result.digest == expected_identity_digest(&identity));
    CHECK(result.digest != 0u);
    CHECK(config.game_digest == result.digest);
    CHECK(config.max_vblanks == 3508u);
    CHECK(config.authenticated_producer_address == UINT32_C(0x800764B4));
    SEED_CONFIG(config);
    result = xg_native_render_baseline_configure(NULL, 3508u, &config);
    CHECK(!result.success);
    CHECK_ZERO_CONFIG(config);
    SEED_CONFIG(config);
    result = xg_native_render_baseline_configure(&identity, 0u, &config);
    CHECK(!result.success);
    CHECK(result.reason == XG_NATIVE_RENDER_BASELINE_INVALID_VBLANK_BOUND);
    CHECK_ZERO_CONFIG(config);

    for (index = 0; index < sizeof(identity); ++index) {
        PsxGameIdentity mismatch = identity;
        ((uint8_t *)&mismatch)[index] ^= 1u;
        SEED_CONFIG(config);
        result = xg_native_render_baseline_configure(&mismatch, 3508u, &config);
        CHECK(!result.success);
        CHECK(result.reason == XG_NATIVE_RENDER_BASELINE_INVALID_IDENTITY);
        CHECK(result.digest == 0u);
        CHECK_ZERO_CONFIG(config);
    }

    for (index = 0; index < 8u; ++index) {
        ram[0x000af880u + index] = (uint8_t)(0x10u + index);
        ram[0x000af890u + index] = (uint8_t)(0x20u + index);
    }
    for (index = 0; index < 32u; ++index) {
        ram[0x000af990u + index] = (uint8_t)(0x30u + index);
        ram[0x000afa64u + index] = (uint8_t)(0x50u + index);
    }
    write_u32_le(ram + 0x000afb0cu, 2u);
    write_u32_le(ram + 0x000afb10u, 0x80100000u);
    for (index = 0; index < 2u * 0x5cu; ++index)
        ram[actor_phys + index] = (uint8_t)(index * 3u + 1u);

    result = xg_native_render_baseline_sample(ram, ram_size);
    repeated = xg_native_render_baseline_sample(ram, ram_size);
    CHECK(result.success && repeated.success);
    CHECK(result.digest != 0u && result.digest == repeated.digest);
    original_digest = result.digest;

    ram[0x000af880u] ^= 1u;
    CHECK(xg_native_render_baseline_sample(ram, ram_size).digest != original_digest);
    ram[0x000af880u] ^= 1u;
    ram[0x000af890u] ^= 1u;
    CHECK(xg_native_render_baseline_sample(ram, ram_size).digest != original_digest);
    ram[0x000af890u] ^= 1u;
    ram[0x000af990u] ^= 1u;
    CHECK(xg_native_render_baseline_sample(ram, ram_size).digest != original_digest);
    ram[0x000af990u] ^= 1u;
    ram[0x000afa64u] ^= 1u;
    CHECK(xg_native_render_baseline_sample(ram, ram_size).digest != original_digest);
    ram[0x000afa64u] ^= 1u;
    ram[actor_phys + 0x5cu] ^= 1u;
    CHECK(xg_native_render_baseline_sample(ram, ram_size).digest != original_digest);
    ram[actor_phys + 0x5cu] ^= 1u;
    write_u32_le(ram + 0x000afb0cu, 1u);
    CHECK(xg_native_render_baseline_sample(ram, ram_size).digest != original_digest);
    write_u32_le(ram + 0x000afb0cu, 2u);

    write_u32_le(ram + 0x000afb0cu,
                 XG_NATIVE_RENDER_BASELINE_ACTOR_COUNT_CAP + 1u);
    result = xg_native_render_baseline_sample(ram, ram_size);
    CHECK(!result.success);
    CHECK(result.reason == XG_NATIVE_RENDER_BASELINE_INVALID_ACTOR_COUNT);

    write_u32_le(ram + 0x000afb0cu, 1u);
    write_u32_le(ram + 0x000afb10u, 0x80200000u);
    result = xg_native_render_baseline_sample(ram, ram_size);
    CHECK(!result.success);
    CHECK(result.reason == XG_NATIVE_RENDER_BASELINE_INVALID_ACTOR_POINTER);

    write_u32_le(ram + 0x000afb10u, 0x801fffd0u);
    result = xg_native_render_baseline_sample(ram, ram_size);
    CHECK(!result.success);
    CHECK(result.reason == XG_NATIVE_RENDER_BASELINE_ACTOR_RANGE_OVERFLOW);

    CHECK(!xg_native_render_baseline_sample(NULL, ram_size).success);
    CHECK(!xg_native_render_baseline_sample(ram, ram_size - 1u).success);
    free(ram);
    return 0;
}
