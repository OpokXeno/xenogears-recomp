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
            f"        {len(variant.source_sites)}u,",
            "        {",
            *(f"            {{UINT32_C(0x{site.pc:08x}), UINT32_C(0x{site.instruction:08x}), "
              f"{ {'read': 0, 'write': 1, 'swc2': 2, 'call': 3, 'bucket': 4}[site.operation] }u, "
              f"{site.width}u, { {'effective-address': 0, 'none': 1, 'result-register': 2}[site.auxiliary] }u}}," for site in variant.source_sites),
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
