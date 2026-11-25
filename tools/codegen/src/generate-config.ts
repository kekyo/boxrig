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
const sdk = template.replace(
  /^#ifndef (BOXRIG_FEATURE_\w+)\n#define \1 [01]\n#endif$/gmu,
  (_, name: string) => {
    if (!(name in values)) throw new Error(`Missing build value for ${name}`);
    return `#if defined(${name}) && ${name} != {{${name}}}
#error "${name} conflicts with the installed boxrig library"
#endif
#ifndef ${name}
#define ${name} {{${name}}}
#endif`;
  }
);
await mkdir(dirname(output), { recursive: true });
await writeFile(output, await renderCodeTemplate(sdk, values, source));
