import { execFile } from 'node:child_process';
import {
  cp,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { afterEach, beforeEach, expect, test } from 'vitest';

const execute = promisify(execFile);
const repository = fileURLToPath(new URL('../../../', import.meta.url));
let root = '';

beforeEach(async () => {
  root = await mkdtemp(resolve(tmpdir(), 'boxrig-codegen-'));
  // Run the built tools in a fresh layout containing only their own package.
  // This also exercises runtime module resolution outside the source tree.
  await cp(
    resolve(repository, 'build/tools/codegen'),
    resolve(root, 'build/tools/codegen'),
    { recursive: true }
  );
  await mkdir(resolve(root, 'tools/codegen'), { recursive: true });
  await cp(
    resolve(repository, 'tools/codegen/package.json'),
    resolve(root, 'tools/codegen/package.json')
  );
  await symlink(
    resolve(repository, 'tools/codegen/node_modules'),
    resolve(root, 'tools/codegen/node_modules')
  );
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

test('generators can execute from an external build directory', async () => {
  await cp(
    resolve(root, 'build/tools/codegen'),
    resolve(root, 'external/sdk/cache/tools'),
    {
      recursive: true,
    }
  );
  await writeFile(
    resolve(root, 'protocol.xml'),
    '<protocol><copyright>Copyright C</copyright></protocol>'
  );
  await execute(
    process.execPath,
    [
      'external/sdk/cache/tools/generate-wayland-notices.mjs',
      'notices.md',
      '1.48',
      'protocol.xml',
    ],
    { cwd: root }
  );
  expect(await readFile(resolve(root, 'notices.md'), 'utf8')).toContain(
    'Copyright C'
  );
});

test('notices retain every upstream copyright block and its source', async () => {
  const notices = [
    '\nCopyright A\nPermission & conditions.\n',
    '\nCopyright B\n',
  ];
  const paths = ['deps/wayland-protocols/stable/a.xml', 'resources/b.xml'];
  for (const [index, path] of paths.entries()) {
    await mkdir(resolve(root, path, '..'), { recursive: true });
    await writeFile(
      resolve(root, path),
      `<protocol><copyright>${notices[index]}</copyright></protocol>`
    );
  }
  await execute(
    process.execPath,
    [
      'build/tools/codegen/generate-wayland-notices.mjs',
      'notices.md',
      '1.47',
      ...paths,
    ],
    { cwd: root }
  );
  const output = await readFile(resolve(root, 'notices.md'), 'utf8');
  for (const notice of notices) expect(output).toContain(notice);
  expect(output).toContain('wayland-protocols/-/tree/1.47');
  expect(output).toContain('## stable/a.xml');
  expect(output).toContain('## resources/b.xml');
});

test('a protocol without a copyright block fails without publishing notices', async () => {
  await writeFile(resolve(root, 'protocol.xml'), '<protocol/>');
  await expect(
    execute(
      process.execPath,
      [
        'build/tools/codegen/generate-wayland-notices.mjs',
        'notices.md',
        '1.47',
        'protocol.xml',
      ],
      { cwd: root }
    )
  ).rejects.toThrow('Missing protocol copyright notice');
  await expect(readFile(resolve(root, 'notices.md'))).rejects.toMatchObject({
    code: 'ENOENT',
  });
});
