"""Independent byte oracles for the single instruction-selection pipeline."""

import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from compile_field_scripts import compile_xgs
from editable_field_scripts import render_source
from extract_disc_field_scripts import parse_scripts_file
from field_instruction_codec import Operand
from validate_field_script_corpus import comment_variants


def script(code):
    return bytes(128) + struct.pack("<I", 1) + bytes(64) + bytes.fromhex(code)


def source_of(binary):
    return render_source(7, binary, parse_scripts_file(binary))[0]


def body(binary):
    return binary[parse_scripts_file(binary)["bytecode_offset"]:]


def insert_before(source, text):
    lines = source.splitlines()
    index = next(i for i, line in enumerate(lines) if text in line.split("//", 1)[0])
    lines.insert(index, "        nop;")
    return "\n".join(lines) + "\n"


class SourceFidelityTests(unittest.TestCase):
    def test_template_operand_values_are_discarded_before_writing(self):
        source = source_of(script('35 10 04 01 00 C0 00'))
        original_write = Operand.write
        observed = []

        def checked_write(operand, raw, text, variables, labels):
            if operand.kind == 'masked' and operand.offset == 3:
                self.assertEqual(raw[3:5], b'\0\0')
                self.assertEqual(raw[5] & 0x40, 0)
                observed.append(text)
            return original_write(operand, raw, text, variables, labels)

        with patch.object(Operand, 'write', checked_write):
            self.assertEqual(compile_xgs(source).build(), script('35 10 04 01 00 C0 00'))
        self.assertIn('1', observed)

    def test_no_whole_program_restoration_for_either_source(self):
        original = script("35 10 04 01 00 C0 00")
        source = source_of(original)
        edited = source.replace(', 1, reserved_flags=128', ', 2, reserved_flags=128', 1)
        self.assertNotEqual(source, edited)
        # Calling either function during compilation would indicate the old
        # reconstruct/re-decompile/return-original path, not instruction encoding.
        with (patch('compile_field_scripts.disassemble', side_effect=AssertionError('binary restoration')),
              patch('editable_field_scripts.render_source', side_effect=AssertionError('source identity bypass'))):
            for text, value in ((source, 1), (edited, 2)):
                ir = compile_xgs(text)
                self.assertFalse(hasattr(ir, 'preserved_binary'))
                linked = ir.link()
                self.assertEqual(linked.link_report['encoding_pipeline'], 'per-instruction-relaxation')
                expected = bytes.fromhex('35 10 04') + struct.pack('<H', value) + bytes.fromhex('C0 00')
                self.assertEqual(body(linked.build('exact')), expected)

    def test_assignment_ignores_all_trace_bits(self):
        original = script("35 10 04 01 00 C0 00")
        source = source_of(original)
        poisoned = source.replace('35 10 04 01 00 C0', '35 00 06 63 00 80')
        # The unused bit 80 comes from the named source argument, not a comment.
        expected = original
        self.assertIn('reserved_flags=128', source)
        self.assertEqual(compile_xgs(source).build(), expected)
        self.assertEqual(compile_xgs(poisoned).build(), expected)
        edited = source.replace('reserved_flags=128', 'reserved_flags=0')
        self.assertEqual(compile_xgs(edited).build(), script('35 10 04 01 00 40 00'))

    def test_ir_edit_is_encoded_without_switching_pipeline(self):
        ir = compile_xgs(source_of(script("35 10 04 01 00 C0 00")))
        assignment = next(record for record in ir.records if record.raw[:1] == b'\x35')
        assignment.raw = bytearray(assignment.raw)
        assignment.raw[3] = 3
        self.assertEqual(body(ir.build()), bytes.fromhex('35 10 04 03 00 C0 00'))

    def test_event_edit_retains_native_party_gate_when_layout_is_valid(self):
        original = script('FE 18 02 13 13 00')
        source = source_of(original)
        self.assertEqual(compile_xgs(source).build(), original)
        edited = source.replace('update         -> L_0000;', 'update         -> L_0003;', 1)
        expected = bytearray(original)
        struct.pack_into('<H', expected, 0x84 + 2, 3)
        linked = compile_xgs(edited).link()
        self.assertEqual(linked.build('exact'), bytes(expected))
        self.assertEqual(linked.link_report['relaxed_choices'], [])

    def test_party_gate_relocates_both_actual_branches(self):
        source = source_of(script('FE 18 02 13 13 00'))
        edited = insert_before(source, 'nop;')
        linked = compile_xgs(edited).link()
        code = body(linked.build('exact'))
        self.assertEqual(code[:3], bytes.fromhex('FE 18 02'))
        self.assertEqual(code[3], 2)
        self.assertEqual(code[5], 1)
        self.assertEqual(struct.unpack_from('<H', code, 6)[0], linked.labels['L_0005'])
        self.assertEqual(struct.unpack_from('<H', code, 9)[0], linked.labels['L_0003'])
        self.assertNotEqual(struct.unpack_from('<h', code, 4)[0], struct.unpack_from('<h', code, 6)[0])
        self.assertTrue(linked.link_report['relaxed_choices'])

    def test_mecha_sharing_checks_new_encoded_values(self):
        original = script('FE 5C 01 35 10 04 01 80 40 00')
        source = source_of(original)
        self.assertEqual(compile_xgs(source).build(), original)
        moved = insert_before(source, 'actor.load_current_actor_mecha')
        self.assertEqual(body(compile_xgs(moved).build()), b'\x13' + body(original))
        different = source.replace(' = -32767;', ' = -32766;', 1)
        linked = compile_xgs(different).link()
        code = body(linked.build('exact'))
        self.assertEqual(code[:8], bytes.fromhex('FE 5C 01 01 08 00 01 80'))
        self.assertEqual(code[8:], bytes.fromhex('35 10 04 02 80 40 00'))
        consistent = different.replace('actor.load_current_actor_mecha(1, 1)', 'actor.load_current_actor_mecha(1, 2)', 1)
        self.assertEqual(body(compile_xgs(consistent).build()), bytes.fromhex('FE 5C 01 35 10 04 02 80 40 00'))

    def test_backward_operands_are_checked_after_layout(self):
        original = script('10 00 01 00 02 00 03 00 E0 10 01 00')
        source = source_of(original)
        self.assertEqual(compile_xgs(source).build(), original)
        edited = source.replace('movement.begin_actor_move(1, 2, 3)', 'movement.begin_actor_move(4, 2, 3)', 1)
        linked = compile_xgs(edited).link()
        code = body(linked.build('exact'))
        self.assertEqual(struct.unpack_from('<h', code, 2)[0], 4)
        self.assertEqual(code[9], 1)  # Jump over the continuation's private descriptor.
        continuation = struct.unpack_from('<H', code, 10)[0]
        self.assertEqual(code[continuation:continuation + 2], b'\x10\x01')
        self.assertEqual(struct.unpack_from('<hhh', code, continuation - 7), (1, 2, 3))
        self.assertTrue(linked.link_report['relaxed_choices'])

    def test_external_mask_uses_only_its_semantic_bits(self):
        original = script('FE D7 01 00 02 00 00 35 10 04 C0 00 40 00')
        source = source_of(original)
        self.assertEqual(compile_xgs(source).build(), original)
        same_modes = source.replace(' = 192;', ' = 193;', 1)
        same_linked = compile_xgs(same_modes).link()
        self.assertEqual(same_linked.link_report['relaxed_choices'], [])
        changed_modes = source.replace(' = 192;', ' = 128;', 1)
        linked = compile_xgs(changed_modes).link()
        code = body(linked.build('exact'))
        self.assertEqual(code[:7], bytes.fromhex('FE D7 01 00 02 00 00'))
        self.assertEqual(code[10] & 0xC0, 0xC0)
        self.assertEqual(code[11:], bytes.fromhex('35 10 04 80 00 40 00'))
        self.assertTrue(linked.link_report['relaxed_choices'])

    def test_readonly_code_view_materializes_after_code_changes(self):
        original = script('48 00 00 10 04 12 04 00')
        source = source_of(original)
        linked = compile_xgs(source).link()
        self.assertEqual(linked.build('exact'), original)
        self.assertEqual(linked.link_report['shared_data_views'], 1)
        edited = insert_before(source, 'state.write_script_u8_to_variable')
        linked = compile_xgs(edited).link()
        code = body(linked.build('exact'))
        self.assertEqual(code[:2], b'\x13\x48')
        base = struct.unpack_from('<H', code, 2)[0]
        self.assertEqual(code[base:base + 8], body(original))
        self.assertEqual(linked.link_report['shared_data_views'], 0)

    def test_terminal_entry_is_independent_of_operand_and_trace_address(self):
        original = bytearray(script('35 10 04 00 00 40 00'))
        struct.pack_into('<H', original, 0x84 + 2, 3)
        original = bytes(original)
        source = source_of(original)
        edited = source.replace(' = 0;', ' = 1;', 1)
        self.assertIn('alias L_0003 = L_0000 + 3 fallback stop;', source)
        self.assertEqual(compile_xgs(source).build(), original)
        self.assertNotEqual(source, edited)
        for text, value in ((source, 0), (edited, 1)):
            linked = compile_xgs(text).link()
            result = linked.build('exact')
            code = body(result)
            self.assertEqual(struct.unpack_from('<H', code, 3)[0], value)
            update = struct.unpack_from('<H', result, 0x84 + 2)[0]
            if value:
                self.assertNotEqual(update, 3)
            else:
                self.assertEqual(update, 3)
            self.assertEqual(code[update], 0)
            self.assertEqual(update, linked.labels['L_0003'])

    def test_comments_cannot_change_encoding_layout_or_events(self):
        cases = [
            '35 10 04 01 00 C0 00',  # unused encoding bit
            'FE 00 00',  # a NOP spelling different from the canonical primary
            'FE 18 02 13 13 00',  # both party continuations
            'FE 5C 01 35 10 04 01 80 40 00',  # forward operand sharing
            '10 00 01 00 02 00 03 00 E0 10 01 00',  # backward sharing
            'FE D7 01 00 02 00 00 35 10 04 C0 00 40 00',  # external mask
            '48 00 00 10 04 12 04 00',  # code read as data
        ]
        for code in cases:
            source = source_of(script(code))
            edited = insert_before(source, 'stop;')
            for text in (source, edited):
                expected = compile_xgs(text).build()
                for name, variant in comment_variants(text).items():
                    with self.subTest(code=code, variant=name, edited=text == edited):
                        self.assertEqual(compile_xgs(variant).build(), expected)

    def test_comments_cannot_make_invalid_code_compile(self):
        valid = source_of(script('35 10 04 01 00 C0 00'))
        source = valid.replace(', 1, reserved_flags=128', ', nonexistent, reserved_flags=128', 1)
        self.assertNotEqual(source, valid)
        for text in (source, *comment_variants(source).values()):
            with self.assertRaisesRegex(ValueError, 'nonexistent'):
                compile_xgs(text)

    def test_lossless_format_fields_and_dispatch_variants(self):
        cases = [
            'FE BD 01 80 00 80 00 80 00',
            'FE 84 05 80 00 80 64 82 01 80 00',
            '47 01 55 80 00 80 00',
            'FE 77 01 42 00 40 01 00 01 FF 00 F0 00',
            'FE D2 04 80 00',
            'FE 5C 02 20 80 00',
            '1C 02 E4 FD 00',
            'FE 0B 02 08 00',
            '02 10 04 40 00 49 08 00 00',
            'FF 00',
            'FE 00',
            '57 00 01 00 02 00 03 00 00 00 E0 57 8F 00',
            '11 00 01 00 02 00 03 00 E0 11 01 0A 80 00',
            '06 07 00 34 12 00 00 0D',
        ]
        for code in cases:
            original = script(code)
            source = source_of(original)
            for name, text in {'normal': source, **comment_variants(source)}.items():
                with self.subTest(code=code, comments=name):
                    self.assertEqual(compile_xgs(text).build(), original)
            plain = comment_variants(source)['no_comments']
            self.assertNotIn('raw(', plain)
            self.assertNotIn('@encoding', plain)
            self.assertNotIn('@layout', plain)

    def test_reserved_fields_cannot_override_semantic_flags(self):
        source = source_of(script('35 10 04 01 00 C0 00'))
        source = source.replace('reserved_flags=128', 'reserved_flags=64')
        with self.assertRaisesRegex(ValueError, 'reserved flags overlap'):
            compile_xgs(source)


if __name__ == '__main__':
    unittest.main()
