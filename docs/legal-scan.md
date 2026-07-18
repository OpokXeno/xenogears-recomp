# Legal gate

`tools/legal_scan.py` runs as a pre-commit hook (master plan 1.2).
It rejects staged files containing PS-X EXE / VAG / TIM magic, BIOS-sized
(524288-byte) files, disc images (*.cue/*.bin), and overlay_captures.json.
Bypass is never acceptable; fix the file, not the scanner.
