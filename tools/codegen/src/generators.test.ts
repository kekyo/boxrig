import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
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

test('generated tables decode entries and choose the permitted encoding pointer', async () => {
  const index =
    '0\t0x3000\tIDEOGRAPHIC SPACE\n8272\t0x3001\tDUPLICATE\n9000\t0x3001\tIDEOGRAPHIC COMMA\n';
  await mkdir(resolve(root, 'data/encoding/whatwg'), { recursive: true });
  await mkdir(resolve(root, 'src/core'), { recursive: true });
  await writeFile(
    resolve(root, 'data/encoding/whatwg/index-jis0208.txt'),
    index
  );
  await writeFile(
    resolve(root, 'data/encoding/whatwg/source.json'),
    JSON.stringify({
      files: [
        {
          file: 'index-jis0208.txt',
          sha256: createHash('sha256').update(index).digest('hex'),
        },
      ],
    })
  );
  await execute(
    process.execPath,
    ['build/tools/codegen/generate-shift-jis.mjs', '.'],
    { cwd: root }
  );
  await writeFile(
    resolve(root, 'consumer.c'),
    `
#include <assert.h>
#include <stdint.h>
#include "src/core/shift_jis_table.h"
int main(void)
{
    assert(shift_jis_decode[0] == 0x3000);
    assert(shift_jis_decode[1] == 0);
    assert(shift_jis_decode[8272] == 0x3001);
    assert(shift_jis_decode[9000] == 0x3001);
    assert(sizeof(shift_jis_encode) / sizeof(*shift_jis_encode) == 2);
    assert(shift_jis_encode[0] == UINT32_C(0x30000000));
    assert(shift_jis_encode[1] == ((UINT32_C(0x3001) << 16) | 9000));
    return 0;
}
`
  );
  await execute(
    'cc',
    ['-std=c99', '-Wall', '-Wextra', '-Werror', 'consumer.c', '-o', 'consumer'],
    { cwd: root }
  );
  await execute(resolve(root, 'consumer'), [], { cwd: root });
});

test('a modified source index fails without publishing a table', async () => {
  await mkdir(resolve(root, 'data/encoding/whatwg'), { recursive: true });
  await mkdir(resolve(root, 'src/core'), { recursive: true });
  await writeFile(
    resolve(root, 'data/encoding/whatwg/index-jis0208.txt'),
    'modified'
  );
  await writeFile(
    resolve(root, 'data/encoding/whatwg/source.json'),
    JSON.stringify({
      files: [{ file: 'index-jis0208.txt', sha256: 'incorrect' }],
    })
  );
  await expect(
    execute(
      process.execPath,
      ['build/tools/codegen/generate-shift-jis.mjs', '.'],
      { cwd: root }
    )
  ).rejects.toThrow('does not match its recorded hash');
  await expect(
    readFile(resolve(root, 'src/core/shift_jis_table.h'))
  ).rejects.toMatchObject({ code: 'ENOENT' });
});
