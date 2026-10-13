import prettierMax from 'prettier-max';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    fileParallelism: false,
    hookTimeout: 180_000,
    reporters: ['verbose'],
    // Shared physical agents can remain busy while another test run owns them.
    // Remote operations and state waits keep their individual deadlines; the
    // test runner must not abandon a pending connection and start another case.
    testTimeout: 0,
    // Native transactions run before desktop automation so graphics failures
    // are reported before a focus/resize operation can enter an unresponsive HWND.
    projects: [
      {
        extends: true,
        test: {
          name: 'graphics',
          include: ['src/win32-d3d11-*.test.ts'],
          sequence: { groupOrder: 1 },
        },
      },
      {
        extends: true,
        test: {
          name: 'cli-opacity',
          include: ['src/win32-cli-opacity.test.ts'],
          sequence: { groupOrder: 2 },
        },
      },
      {
        extends: true,
        test: {
          name: 'desktop',
          include: ['src/**/*.test.ts'],
          exclude: [
            'src/win32-d3d11-*.test.ts',
            'src/win32-cli-opacity.test.ts',
          ],
          sequence: { groupOrder: 3 },
        },
      },
    ],
  },
});
