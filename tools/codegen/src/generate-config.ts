import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname } from 'node:path';
import { renderCodeTemplate } from './funcity-template.js';

const [source, output, ...definitions] = process.argv.slice(2);
const template = await readFile(source, 'utf8');
const values: Record<string, string> = {};
for (const definition of definitions) {
  const match = /^(BOXRIG_FEATURE_[A-Z0-9_]+)=([01])$/u.exec(definition);
  if (!match) throw new Error(`Invalid feature setting ${definition}`);
  values[match[1]] = match[2];
}
// Lock each advertised feature to the value used to build this SDK.
const settings = Object.keys(values)
  .map((name) =>
    [
      `#if defined(${name}) && ${name} != {{${name}}}`,
      `#error "${name} conflicts with the installed boxrig library"`,
      '#endif',
      `#ifndef ${name}`,
      `#define ${name} {{${name}}}`,
      '#endif',
    ].join('\n')
  )
  .join('\n');
const sdk = template.replace(
  '#define BOXRIG_CONFIG_H',
  '#define BOXRIG_CONFIG_H\n\n' + settings
);
await mkdir(dirname(output), { recursive: true });
await writeFile(output, await renderCodeTemplate(sdk, values, source));
