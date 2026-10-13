/* Assert that held output becomes visible, then accepts further native input. */
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [driver, frontend, sample, helper, directory] = process.argv.slice(2);
assert(driver && frontend && sample && helper && directory);
const output = resolve(directory);
mkdirSync(output, { recursive: true });
const video = resolve(output, 'sync-recovery.mp4');
const log = execFileSync(
  driver,
  [frontend, sample, helper, '--sync-recovery'],
  {
    encoding: 'utf8',
    timeout: 120000,
    env: { ...process.env, BOXRIG_TEST_VIDEO: video },
  }
);
writeFileSync(resolve(output, 'driver.log'), log);
const coordinates = /SYNC_VIDEO_PIXEL (\d+) (\d+)/.exec(log);
assert(coordinates, 'native driver must report the observed screen coordinate');
const [, x, y] = coordinates;
const pixels = execFileSync(
  'ffmpeg',
  [
    '-v',
    'error',
    '-i',
    video,
    '-vf',
    `format=rgb24,crop=1:1:${x}:${y}`,
    '-f',
    'rawvideo',
    '-pix_fmt',
    'rgb24',
    'pipe:1',
  ],
  { timeout: 30000 }
);
assert.equal(pixels.length % 3, 0);
const colors = [0x113355, 0x993322, 0x997722];
const runs = [];
for (let index = 0; index < pixels.length; index += 3) {
  const state = colors.findIndex((color) =>
    [16, 8, 0].every(
      (shift, channel) =>
        Math.abs(((color >> shift) & 255) - pixels[index + channel]) <= 8
    )
  );
  if (state < 0) continue;
  const last = runs.at(-1);
  if (last && last.state === state) {
    last.frames++;
    last.last = index / 3;
  } else {
    runs.push({ state, first: index / 3, last: index / 3, frames: 1 });
  }
}
assert.deepEqual(
  runs.map((run) => run.state),
  [0, 1, 2],
  'initial, timeout-recovered and subsequent input frames must appear in order'
);
for (const run of runs)
  assert(run.frames >= 6, 'each state must persist in the video');
const result = {
  frontend,
  pixel: [Number(x), Number(y)],
  frames: pixels.length / 3,
  frames_per_second: 15,
  color_tolerance: 8,
  runs,
  sha256: createHash('sha256').update(readFileSync(video)).digest('hex'),
};
writeFileSync(
  resolve(output, 'results.json'),
  `${JSON.stringify(result, null, 2)}\n`
);
console.log(
  `PASS ${frontend} synchronized output recovery across ${result.frames} video frames`
);
