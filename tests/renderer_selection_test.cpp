#include "launcher_renderer.h"
#undef NDEBUG
#include <cassert>
#include <initializer_list>

int main() {
    // Hardware-only UI indices must round-trip through the stable settings IDs.
    for (int renderer : {1, 2}) {
        const int index = psx_launcher_renderer_index(renderer, true);
        assert(index == renderer - 1);
        assert(psx_launcher_renderer_backend(index, true) == renderer);
    }
    // An old software preference cannot select Vulkan or an empty UI entry.
    assert(psx_launcher_renderer_index(0, true) == 0);
    assert(psx_launcher_renderer_index(-1, true) == 0);
    assert(psx_launcher_renderer_index(99, true) == 0);
    assert(psx_launcher_renderer_backend(-1, true) == 1);
    assert(psx_launcher_renderer_backend(99, true) == 1);
    // Non-opted-in titles retain their legacy Software/OpenGL indices.
    for (int renderer : {0, 1}) {
        assert(psx_launcher_renderer_index(renderer, false) == renderer);
        assert(psx_launcher_renderer_backend(renderer, false) == renderer);
    }
}
