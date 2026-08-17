#ifndef XG_WORLD_DECORATIONS_SOURCE_CAPTURE_H
#define XG_WORLD_DECORATIONS_SOURCE_CAPTURE_H

#include "xg_world_decorations.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_READS =
        39 + 2 * XG_WORLD_DECORATIONS_POSITION_CAPACITY,
    XG_WORLD_DECORATIONS_MAX_AUTHENTICATED_BYTES =
        122 + 8 * XG_WORLD_DECORATIONS_POSITION_CAPACITY,
    XG_WORLD_DECORATIONS_NATIVE_GRID_CELL_COUNT = 25,
    XG_WORLD_DECORATIONS_NATIVE_GRID_LOOKUP_COUNT = 81,
    XG_WORLD_DECORATIONS_NATIVE_SOURCE_DESCRIPTOR_COUNT = 256,
    XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT = 0x1000,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE = 0x28,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_TAG_WORD_COUNT = 9,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_FULL_WORD_WRITE_MASK = 0x155,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_XY0_OFFSET = 0x08,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_CLUT_OFFSET = 0x0e,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_XY1_OFFSET = 0x10,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_XY2_OFFSET = 0x18,
    XG_WORLD_DECORATIONS_NATIVE_PACKET_XY3_OFFSET = 0x20,
    XG_WORLD_DECORATIONS_NATIVE_SCRATCH_CAMERA_OFFSET = 0x28,
    XG_WORLD_DECORATIONS_NATIVE_SCRATCH_DECORATION_OFFSET = 0x48,
    XG_WORLD_DECORATIONS_NATIVE_SCRATCH_CLUT_OFFSET = 0x68,
    XG_WORLD_DECORATIONS_NATIVE_MAX_AUTHENTICATED_READS =
        25747,
    XG_WORLD_DECORATIONS_NATIVE_MAX_AUTHENTICATED_BYTES =
        102850,
};

#define XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC UINT32_C(0x8008615c)
#define XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN UINT32_C(0x80071ab8)
#define XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS UINT32_C(0x1f800000)
#define XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS \
    UINT32_C(0x8009be04)

typedef bool (*XgWorldDecorationsReadU16)(void *context, uint32_t address,
                                          uint16_t *out_value);
typedef bool (*XgWorldDecorationsReadU32)(void *context, uint32_t address,
                                          uint32_t *out_value);

typedef struct XgWorldDecorationsAuthenticatedReader {
    void *context;
    XgWorldDecorationsReadU16 read_u16;
    XgWorldDecorationsReadU32 read_u32;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldDecorationsAuthenticatedReader;

typedef struct XgWorldDecorationsRasterState {
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    bool dither;
    bool mask_set;
    bool mask_check;
} XgWorldDecorationsRasterState;

typedef struct XgWorldDecorationsCaptureRequest {
    uint64_t authentication_generation;
    uint32_t caller_return;
    uint32_t position_address;
    uint32_t position_count;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    XgWorldDecorationsRasterState raster;
    bool helper_arguments_authenticated;
    bool projection_state_authenticated;
} XgWorldDecorationsCaptureRequest;

typedef struct XgWorldDecorationsCapture {
    XgWorldDecorationsSource source;
    uint64_t authentication_generation;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t position_address;
    uint32_t trig_address;
    int16_t angle;
    bool authenticated;
    bool sealed;
} XgWorldDecorationsCapture;

typedef enum XgWorldDecorationsCaptureResult {
    XG_WORLD_DECORATIONS_CAPTURE_OK = 0,
    XG_WORLD_DECORATIONS_CAPTURE_INVALID_ARGUMENT,
    XG_WORLD_DECORATIONS_CAPTURE_UNAUTHENTICATED,
    XG_WORLD_DECORATIONS_CAPTURE_READ_FAILED,
    XG_WORLD_DECORATIONS_CAPTURE_SOURCE_MISMATCH,
    XG_WORLD_DECORATIONS_CAPTURE_FORBIDDEN_RANGE,
} XgWorldDecorationsCaptureResult;

XgWorldDecorationsCaptureResult xg_world_decorations_source_capture(
    const XgWorldDecorationsCaptureRequest *request,
    const XgWorldDecorationsAuthenticatedReader *reader,
    XgWorldDecorationsCapture *out_capture);

typedef struct XgWorldDecorationsNativeRequest {
    uint64_t authentication_generation;
    uint32_t entry_pc;
    uint32_t caller_return;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    XgWorldDecorationsRasterState raster;
    bool projection_state_authenticated;
} XgWorldDecorationsNativeRequest;

typedef struct XgWorldDecorationsNativeScratch {
    XgHost3dVector vertices[XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT];
    XgHost3dMatrix camera_matrix;
    XgHost3dMatrix decoration_matrix;
    uint16_t depth_clut[XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT];
} XgWorldDecorationsNativeScratch;

typedef struct XgWorldDecorationsNativePreparation {
    XgWorldDecorationsNativeScratch scratch;
    uint64_t authentication_generation;
    uint32_t packet_base;
    uint32_t ot_base;
    uint32_t helper_count;
    uint32_t record_count;
    uint32_t temporal_record_count;
    uint32_t final_shared_count;
    uint32_t shared_count_write_mask;
    uint32_t continuation;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    bool authenticated;
    bool sealed;
} XgWorldDecorationsNativePreparation;

typedef enum XgWorldDecorationsNativeResult {
    XG_WORLD_DECORATIONS_NATIVE_OK = 0,
    XG_WORLD_DECORATIONS_NATIVE_INVALID_ARGUMENT,
    XG_WORLD_DECORATIONS_NATIVE_UNAUTHENTICATED,
    XG_WORLD_DECORATIONS_NATIVE_READ_FAILED,
    XG_WORLD_DECORATIONS_NATIVE_SOURCE_MISMATCH,
    XG_WORLD_DECORATIONS_NATIVE_FORBIDDEN_RANGE,
    XG_WORLD_DECORATIONS_NATIVE_BUILD_FAILED,
    XG_WORLD_DECORATIONS_NATIVE_INVALID_OUTPUT,
} XgWorldDecorationsNativeResult;

/* Prepares the complete outer invocation without mutating guest state. On
 * success records[0..record_count) contains the helper FT4 sequence and the
 * sealed value contract contains every non-packet output needed by a cutover.
 * Neither records nor out_preparation is authoritative after a non-OK result.
 *
 * Guest commit contract:
 * - write vertex x/y/z halfwords (not their pad halfwords) at scratch + 0x00;
 * - write both 0x20-byte matrices and the CLUT halfwords at the named scratch
 *   offsets above;
 * - for each record, preserve untouched FT4 template words, write XY at packet
 *   offsets 0x08/0x10/0x18/0x20 and CLUT at 0x0e, then insert its 9-word tag
 *   at the record's OT bucket in record order;
 * - merge final_shared_count at the shared-count address using
 *   shared_count_write_mask, then continue at continuation. No packet, OT,
 *   scratch, register, or GTE state may be published unless this function
 *   returned OK and the runtime preflight and primitive staging both
 *   completed.
 */
XgWorldDecorationsNativeResult xg_world_decorations_native_prepare(
    const XgWorldDecorationsNativeRequest *request,
    const XgWorldDecorationsAuthenticatedReader *reader,
    XgWorldDecorationsRecord *records, uint32_t record_capacity,
    XgWorldDecorationsNativePreparation *out_preparation);

XgWorldDecorationsNativeResult xg_world_decorations_native_prepare_temporal(
    const XgWorldDecorationsNativeRequest *request,
    const XgWorldDecorationsAuthenticatedReader *reader,
    XgWorldDecorationsRecord *records, uint32_t record_capacity,
    XgWorldDecorationsRecord *temporal_records, uint32_t temporal_capacity,
    XgWorldDecorationsNativePreparation *out_preparation);

#ifdef __cplusplus
}
#endif

#endif
