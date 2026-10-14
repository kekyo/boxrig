import { builtinModules } from 'node:module';
import prettierMax from 'prettier-max';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [prettierMax()],
  build: {
    outDir: process.env.BOXRIG_CODEGEN_OUT_DIR ?? '../../build/tools/codegen',
    emptyOutDir: true,
    lib: {
      entry: {
        'generate-shift-jis': 'src/generate-shift-jis.ts',
        'generate-wayland-notices': 'src/generate-wayland-notices.ts',
        'generate-linux-metadata': 'src/generate-linux-metadata.ts',
      },
      formats: ['es'],
      fileName: (_format, name) => `${name}.mjs`,
    },
    rolldownOptions: {
      // Formatting and type checking intentionally dominate this small build.
      // Keep correctness warnings enabled; only omit the timing advisory.
      onLog: (level, log, defaultHandler) => {
        if (level === 'warn' && log.code === 'PLUGIN_TIMINGS') return;
        defaultHandler(level, log);
      },
      external: [
        ...builtinModules,
        ...builtinModules.map((name) => `node:${name}`),
      ],
    },
  },
  test: {
    include: ['src/**/*.test.ts'],
  },
});
