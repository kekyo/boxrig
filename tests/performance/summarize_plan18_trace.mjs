/* boxrig - Under MIT. */
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const directory = process.argv[2];
if (!directory) throw new Error('expected measurement directory');
const cases = JSON.parse(readFileSync(join(directory, 'cases.json'), 'utf8'));
const result = [];
for (const item of cases) {
  if (item.exit_code !== 0) throw new Error(`incomplete trial ${item.directory}`);
  const lines = readFileSync(join(item.directory, 'terminal.log'), 'utf8').split('\n');
  let firstQueue = null;
  let lastQueue = null;
  let previous = null;
  const frames = [];
  const cairoDraws = [];
  const gpu = { draw: [], copy: [] };
  let queuedRequests = 0;
  for (const line of lines) {
    const fields = Object.fromEntries([...line.matchAll(/([a-z_]+)=([0-9]+)/g)]
      .map((match) => [match[1], Number(match[2])]));
    if (fields.gtk_queue_us !== undefined) {
      firstQueue ??= fields.gtk_queue_us;
      lastQueue = fields.gtk_queue_us;
      queuedRequests++;
    }
    if (fields.gtk_render_start_us !== undefined) {
      frames.push({
        first_queue_wait_us: firstQueue === null ? null : fields.gtk_render_start_us - firstQueue,
        last_queue_wait_us: lastQueue === null ? null : fields.gtk_render_start_us - lastQueue,
        queued_requests: queuedRequests,
      });
      firstQueue = null;
      lastQueue = null;
      queuedRequests = 0;
    }
    if (fields.cairo_draw_cpu_us !== undefined) cairoDraws.push(fields.cairo_draw_cpu_us);
    if (fields.gpu_elapsed_ns !== undefined) {
      const phase = line.match(/gpu_phase=(draw|copy)/)?.[1];
      if (phase) gpu[phase].push({ serial: fields.gpu_serial,
        elapsed_ns: fields.gpu_elapsed_ns });
    }
    if (frames.length === 0) continue;
    const frame = frames.at(-1);
    if (fields.blit_call_us !== undefined) {
      frame.blit_call_us = fields.blit_call_us;
      frame.blit_pixels = fields.pixels;
    }
    if (fields.draw_present_cpu_us !== undefined) {
      frame.draw_present_cpu_us = fields.draw_present_cpu_us;
      frame.changed = fields.changed;
    }
    if (fields.gtk_render_duration_us !== undefined)
      frame.gtk_render_duration_us = fields.gtk_render_duration_us;
    if (fields.cumulative_rows !== undefined) {
      for (const key of ['rows', 'cells', 'batches', 'instances', 'uploads',
        'glyph_uploads', 'dirty_rectangles']) {
        const value = key === 'rows' ? fields.cumulative_rows : fields[key];
        frame[key] = previous === null ? null : value - previous[key];
      }
      previous = {
        rows: fields.cumulative_rows, cells: fields.cells, batches: fields.batches,
        instances: fields.instances, uploads: fields.uploads,
        glyph_uploads: fields.glyph_uploads, dirty_rectangles: fields.dirty_rectangles,
      };
    }
  }
  result.push({ ...item, frames, cairo_draw_cpu_us: cairoDraws, gpu });
}
writeFileSync(join(directory, 'trace-summary.json'), JSON.stringify({
  clock: 'g_get_monotonic_time microseconds',
  queue_wait: 'first and last queue request since preceding render; duplicate requests counted',
  blit: 'CPU call duration, not GPU completion time',
  cumulative_delta: 'null for the first observed frame',
  gpu: 'asynchronous samples grouped by phase and query serial; not paired with individual CPU frames',
  trials: result,
}, null, 2));
