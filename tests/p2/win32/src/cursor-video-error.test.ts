import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { PNG } from 'pngjs';
import { it } from 'vitest';
import { cursorVideoStaticError } from './cursor-video-error.js';

// Source screenshot and decoded frame 90 from the physical amd64/GDI failure.
// Both were visually inspected: static text and the gold marker are unchanged.
const reference = PNG.sync.read(
  readFileSync(
    new URL('../fixtures/cursor-static-reference.png', import.meta.url)
  )
);
const decoded = PNG.sync.read(
  readFileSync(new URL('../fixtures/cursor-static-h264.png', import.meta.url))
);
const rgb = Buffer.alloc(decoded.width * decoded.height * 3);
for (let pixel = 0; pixel < decoded.width * decoded.height; pixel++)
  decoded.data.copy(rgb, pixel * 3, pixel * 4, pixel * 4 + 3);
const cursor = { x: 5, y: 13, width: 11, height: 19 };
const error = (frame: Uint8Array): number =>
  cursorVideoStaticError(
    frame,
    reference.data,
    decoded.width,
    decoded.height,
    cursor
  );

it('accepts retained static content after H.264 encoding', () => {
  assert(error(rgb) <= 48);
});

it('detects the disappearance of every static glyph', () => {
  for (const [row, text] of [
    [0, 'static left and right'],
    [2, 'unchanged outside cursor'],
  ] as const)
    for (let column = 0; column < text.length; column++) {
      if (text[column] === ' ') continue;
      const changed = Buffer.from(rgb);
      for (let y = row * 15; y < (row + 1) * 15; y++)
        for (let x = column * 7; x < (column + 1) * 7; x++)
          changed.fill(
            0,
            (y * decoded.width + x) * 3,
            (y * decoded.width + x) * 3 + 3
          );
      assert(error(changed) > 48, `missing glyph ${row}:${column}`);
    }
});

it('detects a stale cursor outside the current cursor region', () => {
  const changed = Buffer.from(rgb);
  for (let y = 15; y < 30; y++)
    for (let x = 28; x < 35; x++)
      changed.fill(
        255,
        (y * decoded.width + x) * 3,
        (y * decoded.width + x) * 3 + 3
      );
  assert(error(changed) > 48);
});

it('detects disappearance of the gold presentation marker', () => {
  const changed = Buffer.from(rgb);
  for (let y = 45; y < 60; y++)
    for (let x = 0; x < 14; x++)
      changed.fill(
        0,
        (y * decoded.width + x) * 3,
        (y * decoded.width + x) * 3 + 3
      );
  assert(error(changed) > 48);
});
