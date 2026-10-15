#include "xg_render_semantic_presentation.h"

#undef NDEBUG
#include <assert.h>

static uint64_t clock_ns(void *unused) {
    (void)unused;
    return UINT64_C(1000000000);
}

static uint64_t lag(const XgRenderPresenterServices *presenter,
                    uint64_t elapsed, uint64_t debt, bool rebase) {
    assert(xg_render_presenter_sync_source_clock(presenter,
        elapsed * UINT64_C(33868800) / UINT64_C(1000000000),
        UINT64_C(1000000000) + elapsed, debt, true, rebase));
    XgRenderPresentationDiagnostics diagnostics;
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    return diagnostics.visual_lag_ns;
}

int main(void) {
    const XgRenderPresenterServices presenter = {
        .owner_token = UINT64_C(0x534f55524345434c),
        .clock_ns = clock_ns,
    };
    xg_render_semantic_presentation_reset();
    assert(xg_render_presentation_lifecycle_claim_open(presenter.owner_token) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);

    /* Ordinary scheduling noise must not compress each source interval by
     * half a 240 Hz tick. Exercise repeated late/on-time guest quanta. */
    uint64_t previous = lag(&presenter, 0u, UINT64_C(6000000), true);
    for (uint64_t frame = 1u; frame <= 240u; ++frame) {
        const uint64_t current = lag(&presenter,
            frame * UINT64_C(16666667),
            frame & 1u ? 0u : UINT64_C(6000000), false);
        const uint64_t change = previous > current ? previous-current : current-previous;
        assert(change < UINT64_C(100000));
        previous = current;
    }

    /* A real stall still attacks immediately and sheds most of its debt at
     * the original rate; the slow noise band must not retain 250 ms. */
    assert(lag(&presenter, 0u, UINT64_C(250000000), true) == UINT64_C(250000000));
    const uint64_t recovered = lag(&presenter, UINT64_C(1000000000), 0u, false);
    assert(recovered > UINT64_C(150000000));
    assert(recovered < UINT64_C(200000000));

    /* A pause/debt rebase clears the envelope rather than carrying old lag
     * into the new source clock correspondence. */
    assert(lag(&presenter, UINT64_C(2000000000), 0u, true) == 0u);
    xg_render_semantic_presentation_reset();
    return 0;
}
