import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const dataDirectory = resolve(root, 'resources/unicode/17.0.0');
const outputPath = resolve(root, 'src/ui/word_break_data.inc');
const download = process.argv.includes('--download');
const check = process.argv.includes('--check');

const sources = [
  {
    name: 'WordBreakProperty.txt',
    url: 'https://www.unicode.org/Public/17.0.0/ucd/auxiliary/WordBreakProperty.txt',
    sha256: '72274cac1e6b919507db35655c3e175aa27274668a1ece95c28d2069f2ad9852',
  },
  {
    name: 'WordBreakTest.txt',
    url: 'https://www.unicode.org/Public/17.0.0/ucd/auxiliary/WordBreakTest.txt',
    sha256: '1de23a75f37904abc7d206239ee8d34f8fdf0fb4ab32a7174dfbabbde25419b2',
  },
  {
    name: 'emoji-data.txt',
    url: 'https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt',
    sha256: '2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b',
  },
  {
    name: 'LICENSE.txt',
    url: 'https://www.unicode.org/license.txt',
    sha256: 'e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96',
  },
];

const propertyNames = new Map([
  ['ALetter', 'BOXRIG_WORD_BREAK_ALETTER'],
  ['CR', 'BOXRIG_WORD_BREAK_CR'],
  ['Double_Quote', 'BOXRIG_WORD_BREAK_DOUBLE_QUOTE'],
  ['Extend', 'BOXRIG_WORD_BREAK_EXTEND'],
  ['ExtendNumLet', 'BOXRIG_WORD_BREAK_EXTEND_NUM_LET'],
  ['Format', 'BOXRIG_WORD_BREAK_FORMAT'],
  ['Hebrew_Letter', 'BOXRIG_WORD_BREAK_HEBREW_LETTER'],
  ['Katakana', 'BOXRIG_WORD_BREAK_KATAKANA'],
  ['LF', 'BOXRIG_WORD_BREAK_LF'],
  ['MidLetter', 'BOXRIG_WORD_BREAK_MID_LETTER'],
  ['MidNum', 'BOXRIG_WORD_BREAK_MID_NUM'],
  ['MidNumLet', 'BOXRIG_WORD_BREAK_MID_NUM_LET'],
  ['Newline', 'BOXRIG_WORD_BREAK_NEWLINE'],
  ['Numeric', 'BOXRIG_WORD_BREAK_NUMERIC'],
  ['Regional_Indicator', 'BOXRIG_WORD_BREAK_REGIONAL_INDICATOR'],
  ['Single_Quote', 'BOXRIG_WORD_BREAK_SINGLE_QUOTE'],
  ['WSegSpace', 'BOXRIG_WORD_BREAK_WSEG_SPACE'],
  ['ZWJ', 'BOXRIG_WORD_BREAK_ZWJ'],
]);

const digest = (bytes) => createHash('sha256').update(bytes).digest('hex');

const readSource = async (source) => {
  const path = resolve(dataDirectory, source.name);

  if (download) {
    const response = await fetch(source.url);

    if (!response.ok) {
      throw new Error(`Unable to download ${source.url}: ${response.status}`);
    }
    await writeFile(path, new Uint8Array(await response.arrayBuffer()));
  }
  const bytes = await readFile(path);
  const actual = digest(bytes);

  if (actual !== source.sha256) {
    throw new Error(
      `${source.name} SHA-256 mismatch: expected ${source.sha256}, got ${actual}`,
    );
  }
  return bytes.toString('utf8');
};

const parseRange = (text) => {
  const values = text.trim().split('..');
  const first = Number.parseInt(values[0], 16);
  const last = Number.parseInt(values.at(-1), 16);

  if (!Number.isSafeInteger(first) || !Number.isSafeInteger(last)) {
    throw new Error(`Invalid Unicode range: ${text}`);
  }
  return { first, last };
};

const parseProperties = (text) => {
  const ranges = [];

  for (const sourceLine of text.split(/\r?\n/u)) {
    const line = sourceLine.replace(/#.*/u, '').trim();

    if (line.length === 0) {
      continue;
    }
    const fields = line.split(';').map((field) => field.trim());
    const property = propertyNames.get(fields[1]);

    if (property === undefined) {
      throw new Error(`Unknown Word_Break property: ${fields[1]}`);
    }
    ranges.push({ ...parseRange(fields[0]), property });
  }
  ranges.sort((left, right) => left.first - right.first);
  return ranges;
};

const parseExtendedPictographic = (text) => {
  const ranges = [];

  for (const sourceLine of text.split(/\r?\n/u)) {
    const line = sourceLine.replace(/#.*/u, '').trim();

    if (line.length === 0) {
      continue;
    }
    const fields = line.split(';').map((field) => field.trim());

    if (fields[1] === 'Extended_Pictographic') {
      ranges.push(parseRange(fields[0]));
    }
  }
  ranges.sort((left, right) => left.first - right.first);
  return ranges;
};

const hexadecimal = (value) => `UINT32_C(0x${value.toString(16)})`;

const generate = (properties, pictographic) => {
  const lines = [
    '/* Generated from Unicode 17.0.0 data by',
    ' * tools/unicode/generate-word-break.mjs. Do not edit.',
    ' */',
    '',
    'static const boxrig_word_break_range boxrig_word_break_ranges[] = {',
  ];

  for (const range of properties) {
    lines.push(
      `    {${hexadecimal(range.first)}, ${hexadecimal(range.last)}, ${range.property}},`,
    );
  }
  lines.push(
    '};',
    '',
    'static const boxrig_unicode_range boxrig_extended_pictographic_ranges[] = {',
  );
  for (const range of pictographic) {
    lines.push(`    {${hexadecimal(range.first)}, ${hexadecimal(range.last)}},`);
  }
  lines.push('};', '');
  return `${lines.join('\n')}\n`;
};

const propertyText = await readSource(sources[0]);
await readSource(sources[1]);
const emojiText = await readSource(sources[2]);
await readSource(sources[3]);
const generated = generate(
  parseProperties(propertyText),
  parseExtendedPictographic(emojiText),
);

if (check) {
  const existing = await readFile(outputPath, 'utf8');

  if (existing !== generated) {
    throw new Error(`${outputPath} is not reproducible from pinned data`);
  }
} else {
  await writeFile(outputPath, generated);
}
