# Unicode word-break data

`17.0.0/` is a fixed input used for implementing and testing compliance with the default word boundary in UAX #29.

Sources:

- https://www.unicode.org/Public/17.0.0/ucd/auxiliary/WordBreakProperty.txt
- https://www.unicode.org/Public/17.0.0/ucd/auxiliary/WordBreakTest.txt
- https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt
- https://www.unicode.org/license.txt

`node tools/unicode/generate-word-break.mjs --download` retrieves input from the URLs above, verifies the fixed SHA-256, and regenerates `src/ui/word_break_data.inc`. A normal regeneration does not use the network; instead, it reads the checked-in input. `--check` only verifies that the SHA-256 of the input matches that of the generated result and does not modify the file.

The data files and generated tables are subject to the Unicode License V3 as described in `17.0.0/LICENSE.txt`.
