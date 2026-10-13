/* Inspect the linked contract as well as the archives used by software clients. */
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [objdump, nm, directory, reportDirectory] = process.argv.slice(2);
assert.ok(objdump && nm && directory && reportDirectory);
mkdirSync(reportDirectory, { recursive: true });
const inspect = (name) => {
  const output = execFileSync(objdump, ['-p', join(directory, name)], { encoding: 'utf8' });
  writeFileSync(join(reportDirectory, `${name}.txt`), output);
  return output;
};
const exportsFrom = (dump) => {
  const table = dump.match(/\[Ordinal\/Name Pointer\] Table\r?\n([\s\S]*?)(?:\r?\n\r?\n|$)/);
  assert.ok(table, 'PE export name table is required');
  return [...table[1].matchAll(/^\s*\[\s*\d+\]\s+(\S+)\s*$/gm)].map((match) => match[1]);
};
const renderer = inspect('boxrig-render-ogl.dll');
const adapter = inspect('boxrig-win32-ogl.dll');
const rendererExports = exportsFrom(renderer);
const adapterExports = exportsFrom(adapter);
assert.ok(rendererExports.includes('boxrig_ogl_renderer_create'));
assert.ok(rendererExports.includes('boxrig_gl_runtime_create'));
assert.ok(rendererExports.every((name) => /^boxrig_(?:ogl_renderer|gl_runtime)_/.test(name)));
assert.deepEqual(adapterExports.sort(), [
  'boxrig_win32_opengl_terminal_component_create_child',
  'boxrig_win32_opengl_terminal_component_create_on_target',
  'boxrig_wgl_surface_get_info',
  'boxrig_win32_opengl_terminal_component_get_diagnostics',
  'boxrig_wgl_binding_create', 'boxrig_wgl_binding_destroy',
  'boxrig_wgl_surface_create', 'boxrig_wgl_surface_destroy',
  'boxrig_wgl_surface_get_runtime', 'boxrig_wgl_surface_make_current',
  'boxrig_wgl_surface_release_current', 'boxrig_wgl_surface_supports_srgb',
  'boxrig_wgl_surface_swap', 'boxrig_wgl_surface_window',
].sort());
assert.doesNotMatch(renderer, /DLL Name:.*(?:opengl|pango|cairo|fontconfig|conpty)/i);
assert.match(adapter, /DLL Name:\s*OPENGL32\.dll/i);
assert.match(adapter, /DLL Name:\s*boxrig-render-ogl\.dll/i);
assert.doesNotMatch(adapter, /DLL Name:.*(?:pango|cairo|fontconfig|conpty)/i);
for (const library of ['libboxrig.a', 'libboxrig-ui.a', 'libboxrig-win32.a', 'libboxrig-render-ogl.a']) {
  const undefinedSymbols = execFileSync(nm, ['-u', join(directory, library)], { encoding: 'utf8' });
  writeFileSync(join(reportDirectory, `${library}.txt`), undefinedSymbols);
  assert.doesNotMatch(undefinedSymbols, /\b(?:__imp_)?_?(?:wgl[A-Z]|gl[A-Z]|egl[A-Z]|pango_|cairo_|Fc[A-Z])/);
}
const staticClient = inspect('test_wgl_package-static.exe');
const sharedClient = inspect('test_wgl_package-shared.exe');
assert.doesNotMatch(staticClient, /DLL Name:\s*boxrig-/i);
assert.match(sharedClient, /DLL Name:\s*boxrig-render-ogl\.dll/i);
assert.match(sharedClient, /DLL Name:\s*boxrig-win32-ogl\.dll/i);
console.log('PASS optional Win32 OpenGL archives, DLL exports, imports and installed clients');
