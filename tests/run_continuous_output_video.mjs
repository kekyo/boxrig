/* Assert paint, resize and input progress while a stationary pointer observes output. */
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [driver, frontend, sample, helper, directory] = process.argv.slice(2);
assert(driver && frontend && sample && helper && directory);
const output = resolve(directory);
mkdirSync(output, { recursive: true });
const video = resolve(output, 'continuous-output.mp4');
const execution = spawnSync(
  driver,
  [frontend, sample, helper, '--continuous-output'],
  {
    encoding: 'utf8',
    timeout: 120000,
    env: { ...process.env, BOXRIG_TEST_VIDEO: video },
  }
);
const log = (execution.stdout ?? '') + (execution.stderr ?? '');
writeFileSync(resolve(output, 'driver.log'), log);
assert.equal(execution.status, 0, log);

const results = [];
for (const [name, expression, expected] of [
  ['original', /FLOOD_VIDEO_PIXEL (\d+) (\d+)/, [0, 1, 2]],
  ['expanded', /FLOOD_VIDEO_RESIZED_PIXEL (\d+) (\d+)/, [1, 2]],
]) {
  const coordinates = expression.exec(log);
  assert(
    coordinates,
    'native driver must report each observed screen coordinate'
  );
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
  const colors = [0x113355, 0x993322, 0x227744];
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
    expected,
    `${name} region must advance through the observed output and input states`
  );
  for (const run of runs)
    assert(
      run.frames >= 6,
      'each completed state must persist in the recording'
    );
  results.push({
    name,
    pixel: [Number(x), Number(y)],
    frames: pixels.length / 3,
    runs,
  });
}
const record = {
  frontend,
  frames_per_second: 15,
  color_tolerance: 8,
  regions: results,
  sha256: createHash('sha256').update(readFileSync(video)).digest('hex'),
};
writeFileSync(
  resolve(output, 'results.json'),
  JSON.stringify(record, null, 2) + '\n'
);
console.log(
  `PASS ${frontend} continuous output, native resize and input across ${results[0].frames} video frames`
);
