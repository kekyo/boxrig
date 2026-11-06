# Unicode word-break data

[日本語](./README_ja.md)

`18.0.0/` contains fixed input for implementing and testing the [default word boundaries in UAX #29](https://www.unicode.org/reports/tr29/tr29-49.html).
It uses Unicode 18.0.0, matching the utf8proc version used by core.

Sources:

- https://www.unicode.org/Public/18.0.0/ucd/auxiliary/WordBreakProperty.txt
- https://www.unicode.org/Public/18.0.0/ucd/auxiliary/WordBreakTest.txt
- https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt
- https://www.unicode.org/license.txt

`node tools/unicode/generate-word-break.mjs --download` retrieves input from the URLs above, verifies the fixed SHA-256, and regenerates `src/ui/word_break_data.inc`. A normal regeneration does not use the network; instead, it reads the checked-in input. `--check` verifies the input SHA-256 and generated output without modifying any files.

The data files and generated tables are subject to the Unicode License V3 as described in `18.0.0/LICENSE.txt`.
