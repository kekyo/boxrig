import { readFile, writeFile } from 'node:fs/promises';
import { renderCodeTemplate } from './funcity-template.js';

const [output, version, ...paths] = process.argv.slice(2);
if (!output || !version || paths.length === 0)
  throw new Error('Usage: generate-wayland-notices OUTPUT VERSION XML...');

const protocols: { path: string; notice: string }[] = [];
for (const path of paths) {
  const xml = await readFile(path, 'utf8');
  const notice = /<copyright>([\s\S]*?)<\/copyright>/u.exec(xml)?.[1];
  if (!notice?.trim())
    throw new Error(`Missing protocol copyright notice: ${path}`);
  // Preserve each upstream notice, including variants of the MIT/X11 license.
  protocols.push({
    path: path.replace(/^deps\/wayland-protocols\//u, ''),
    notice,
  });
}

await writeFile(
  output,
  await renderCodeTemplate(
    `# Wayland protocol notices

These protocols supply generated code used by boxrig libraries and demos.
Source release: https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tree/{{version}}
Each notice below is copied from its protocol XML without modification.

{{for protocol protocols}}## {{protocol.path}}

{{protocol.notice}}
{{end}}`,
    { version, protocols },
    'Wayland-protocols-NOTICES.md'
  )
);
