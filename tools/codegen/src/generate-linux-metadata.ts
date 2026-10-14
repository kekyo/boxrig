import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { isAbsolute, join, relative } from 'node:path';
import { renderCodeTemplate } from './funcity-template.js';

const [output, version, prefix, libdir, includedir, pcdir, ...packages] =
  process.argv.slice(2);
if (
  !output ||
  !version ||
  !prefix ||
  !libdir ||
  !includedir ||
  !pcdir ||
  packages.length === 0
)
  throw new Error(
    'Usage: generate-linux-metadata OUTPUT VERSION PREFIX LIBDIR INCLUDEDIR PCDIR PACKAGE...'
  );
if (!/^[0-9]+\.[0-9]+\.[0-9]+[a-zA-Z0-9.+-]*$/u.test(version))
  throw new Error('Invalid package version');
for (const path of [prefix, libdir, includedir, pcdir]) {
  if (!isAbsolute(path) || /[\s$#]/u.test(path))
    throw new Error(
      'Installation directories must be absolute paths without whitespace, $ or #'
    );
}
const variables = {
  version,
  epoxy: process.env.BOXRIG_GTK_OPENGL === '0' ? '' : ', epoxy',
  prefix: '${pcfiledir}/' + (relative(pcdir, prefix) || '.'),
  libdir: '${prefix}/' + (relative(prefix, libdir) || '.'),
  includedir: '${prefix}/' + (relative(prefix, includedir) || '.'),
};
await mkdir(output, { recursive: true });
for (const name of packages) {
  if (!/^boxrig(?:-[a-z0-9]+)*$/u.test(name))
    throw new Error('Invalid package name');
  const source = `packaging/linux/${name}.pc.in`;
  await writeFile(
    join(output, `${name}.pc`),
    await renderCodeTemplate(await readFile(source, 'utf8'), variables, source)
  );
}
