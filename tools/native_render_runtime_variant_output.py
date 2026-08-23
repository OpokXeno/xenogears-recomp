from __future__ import annotations

from native_render_manifest_model import Digest32
from native_render_runtime_variant_verify import VerifiedRuntimeVariants


def bytes_initializer(value: Digest32) -> str:
    return ",".join(f"0x{byte:02x}" for byte in value)


def render_c(verified: VerifiedRuntimeVariants) -> bytes:
    contract = verified.contract
    artifact = contract.artifact
    rows: list[str] = []
    for variant in contract.variants:
        rows.extend((
            "    {",
            f"        {contract.canonical.producer_record_id}u, {contract.canonical.site_record_id}u,",
            f"        {{{bytes_initializer(contract.canonical.game_identity)}}},",
            f"        {{{bytes_initializer(contract.canonical.manifest_identity)}}},",
            f"        {{{bytes_initializer(verified.companion_identity)}}},",
            f"        {{{bytes_initializer(artifact.identity.sha256)}}},",
            f"        UINT32_C(0x{artifact.identity.crc32:08x}), {artifact.identity.size}u,",
            f"        UINT32_C(0x{artifact.base_address:08x}),",
            f"        UINT32_C(0x{artifact.base_address + artifact.range_offset:08x}), {artifact.range_size}u,",
            f"        UINT32_C(0x{artifact.range_crc32:08x}), {{{bytes_initializer(artifact.range_identity)}}},",
            f"        UINT32_C(0x{contract.canonical.producer_entry:08x}),",
            f"        UINT32_C(0x{contract.canonical.capture_site:08x}),",
            f"        UINT32_C(0x{contract.canonical.static_callee:08x}),",
            f"        UINT32_C(0x{contract.canonical.return_site:08x}),",
            f"        UINT32_C(0x{variant.activation.window_start:08x}), {variant.activation.window_size}u,",
            f"        {{{bytes_initializer(variant.activation.window_identity)}}},",
            f"        UINT32_C(0x{variant.activation.site:08x}),",
            "        3u,",
            f"        UINT32_C(0x{variant.activation.target:08x}),",
            f"        UINT32_C(0x{variant.activation.delay_instruction:08x}),",
            f"        UINT32_C(0x{variant.physical_producer_entry:08x}),",
            f"        UINT32_C(0x{variant.capture.window_start:08x}), {variant.capture.window_size}u,",
            f"        {{{bytes_initializer(variant.capture.window_identity)}}},",
            f"        UINT32_C(0x{variant.capture.site:08x}),",
            "        3u,",
            f"        UINT32_C(0x{variant.capture.target:08x}),",
            f"        UINT32_C(0x{variant.capture.delay_instruction:08x}),",
            f"        UINT32_C(0x{variant.physical_return_site:08x}),",
            f"        UINT32_C(0x{variant.model_dispatch_window_start:08x}),",
            f"        {variant.model_dispatch_matrix_stack_offset}u,",
            f"        {len(variant.model_dispatch_instructions)}u,",
            "        {",
            *(f"            UINT32_C(0x{instruction:08x})," for instruction in variant.model_dispatch_instructions),
            "        },",
            f"        {len(variant.source_sites)}u,",
            "        {",
            *(f"            {{UINT32_C(0x{site.pc:08x}), UINT32_C(0x{site.instruction:08x}), "
              f"{ {'read': 0, 'write': 1, 'swc2': 2, 'call': 3, 'bucket': 4}[site.operation] }u, "
              f"{site.width}u, { {'effective-address': 0, 'none': 1, 'result-register': 2}[site.auxiliary] }u}}," for site in variant.source_sites),
            "        },",
            f"        {len(variant.native_cutovers)}u,",
            "        {",
            *(f"            {{UINT32_C(0x{cutover.pc:08x}), UINT32_C(0x{cutover.instruction:08x}), "
               f"UINT32_C(0x{cutover.continuation:08x}), "
               f"UINT32_C(0x{cutover.code_range_start:08x}), {cutover.code_range_size}u, "
               f"{{{bytes_initializer(cutover.code_range_identity)}}}, "
               f"{ {'observe': 0, 'local': 1, 'return': 2}[cutover.transfer] }u, "
               f"{ {'actor': 0, 'compass-world': 1, 'compass-screen': 2, 'zoom-rgb-begin': 3, 'zoom-rgb-commit': 4, 'zoom-entry': 5, 'zoom-native': 6, 'zoom-initializer-begin': 7, 'zoom-initializer-commit': 8, 'particle-initializer': 9, 'particle-native': 10, 'resource-initializer-begin': 11, 'resource-initializer-writer': 12, 'resource-initializer-commit': 13, 'zoom-initializer-writer': 14}[cutover.handler] }u}}," for cutover in variant.native_cutovers),
            "        },",
            "    },",
        ))
    lines = (
        '#include "xg_render_runtime_variants_generated.h"',
        "",
        "const XgRenderRuntimeVariantDescriptor xg_render_runtime_variant_descriptors[] = {",
        *rows,
        "};",
        f"const uint32_t xg_render_runtime_variant_descriptor_count = {len(contract.variants)}u;",
        "",
    )
    return "\n".join(lines).encode("ascii")
