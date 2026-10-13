import prettierMax from 'prettier-max';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    fileParallelism: false,
    hookTimeout: 30_000,
    include: ['src/**/*.test.ts'],
    testTimeout: 30_000,
  },
});
