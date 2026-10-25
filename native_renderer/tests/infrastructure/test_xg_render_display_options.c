#include "xg_render_source_commit.h"

#include <assert.h>

int main(void) {
    const XgPresentationIdentity identity = {
        .presentation_epoch = 1u, .source_sequence = 1u,
        .guest_vblank_sequence = 1u, .guest_cycle = 100u,
        .scene_generation = 1u,
    };
    const XgSemanticSceneIdentity scene = {
        .disc_id = 1u, .executable_identity = 1u,
        .module = XG_SEMANTIC_MODULE_WORLD,
    };
    XgSemanticDisplayState display = {
        .width = 320u, .height = 216u, .aspect_num = 4u, .aspect_den = 3u,
        .temporal_hz = 60u,
    };
    XgRenderSourceCommitHandle commits[2];
    XgRenderSourceCommitHeader headers[2];

    xg_render_source_commit_reset();
    for (unsigned i = 0u; i < 2u; ++i) {
        XgRenderSourceBuilder builder;
        display.dithering_disabled = i != 0u;
        assert(xg_render_source_commit_begin(&identity, &scene, &display,
            2u, false, true, &builder) == XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_set_native_work(builder, true) == XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_seal(builder, &commits[i]) == XG_RENDER_SOURCE_COMMIT_OK);
    }
    /* Both frames remain queued with independent options after the host changes
     * its preference. Their identities must not alias in the renderer cache. */
    display.dithering_disabled = false;
    for (unsigned i = 0u; i < 2u; ++i) {
        assert(xg_render_source_commit_header_copy(commits[i], &headers[i]) == XG_RENDER_SOURCE_COMMIT_OK);
        assert(headers[i].display.dithering_disabled == (i != 0u));
        assert(headers[i].display.temporal_hz == 60u);
    }
    assert(headers[0].digest != headers[1].digest);
    for (unsigned i = 0u; i < 2u; ++i)
        assert(xg_render_source_commit_retire(commits[i]) == XG_RENDER_SOURCE_COMMIT_OK);
    /* Fixed dimensions are immutable source options too. Changing the selected
     * resolution must not reuse the preceding raster. */
    display.dithering_disabled = false;
    uint64_t previous_digest = 0u;
    const uint16_t sizes[][2] = {{0,0},{1920,1080},{3840,2160},{801,600}};
    for (unsigned i=0u;i<sizeof(sizes)/sizeof(*sizes);++i) {
        XgRenderSourceBuilder builder;
        XgRenderSourceCommitHandle commit;
        XgRenderSourceCommitHeader header;
        display.render_width=sizes[i][0]; display.render_height=sizes[i][1];
        assert(xg_render_source_commit_begin(&identity,&scene,&display,2u,false,true,&builder)==XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_set_native_work(builder,true)==XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_seal(builder,&commit)==XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_header_copy(commit,&header)==XG_RENDER_SOURCE_COMMIT_OK);
        assert(header.display.render_width==sizes[i][0]&&header.display.render_height==sizes[i][1]);
        assert(header.digest!=previous_digest); previous_digest=header.digest;
        assert(xg_render_source_commit_retire(commit)==XG_RENDER_SOURCE_COMMIT_OK);
    }
    XgRenderSourceBuilder invalid;
    display.render_width=1920u;display.render_height=0u;
    assert(xg_render_source_commit_begin(&identity,&scene,&display,2u,false,true,&invalid)==XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT);
    display.render_height=1080u;display.render_width=16385u;
    assert(xg_render_source_commit_begin(&identity,&scene,&display,2u,false,true,&invalid)==XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT);
    return 0;
}
