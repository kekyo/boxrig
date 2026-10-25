/* Original geometric test fonts, under the project's MIT license.
 * Regenerate with Node after installing opentype.js@1.3.4 in a temporary prefix:
 * node tests/fixtures/generate-width-font.mjs /tmp/prefix/node_modules/opentype.js
 * API: https://github.com/opentypejs/opentype.js#craft-a-font
 */
import { createRequire } from 'node:module';
import { writeFileSync } from 'node:fs';

const { Font, Glyph, Path } = createRequire(import.meta.url)(process.argv[2]);
const glyph = (code, advance, ink) => {
  const path = new Path();
  if (ink) {
    const inkWidth = code < 127 ? advance * 0.6 : advance;
    path.moveTo(0, 0);
    path.lineTo(inkWidth, 0);
    path.lineTo(inkWidth, 700);
    path.lineTo(0, 700);
    path.closePath();
  }
  return new Glyph({ name: code === 0 ? '.notdef' : `uni${code.toString(16)}`,
    unicode: code, advanceWidth: advance, path });
};

for (const [variant, family] of [
  ['fractional', 'Boxrig Width Fixture'],
  ['narrow', 'Boxrig Width Narrow'],
  ['ascii', 'Boxrig Width ASCII'],
]) {
  const glyphs = [glyph(0, 500, false)];
  for (let code = 32; code < 127; code++) glyphs.push(glyph(code, 500, code !== 32));
  if (variant !== 'ascii') {
    for (const [index, width] of [1050, 600, 1000, 500, 2250].entries())
      glyphs.push(glyph(0x2460 + index, variant === 'narrow' ? 500 : width, true));
    glyphs.push(glyph(0x0301, 0, false));
  }
  const font = new Font({ familyName: family, styleName: 'Regular',
    unitsPerEm: 1000, ascender: 800, descender: -200,
    createdTimestamp: 0, glyphs, license: 'MIT',
    description: 'Original rectangles with controlled advances for boxrig tests.' });
  writeFileSync(new URL(`width-${variant}.otf`, import.meta.url), Buffer.from(font.toArrayBuffer()));
}
