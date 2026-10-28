/* boxrig - Under MIT. */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';

const root = process.cwd();
const output = resolve(process.argv[2] ?? 'artifacts/plan11/gtk-matrix');
const directory = join(output, '0-baseline-cairo-h10000-idle-none-a10');
if (!existsSync(join(directory, 'terminal.log'))) {
  spawnSync('bash', [join(root, 'tests/performance/run_annotation_gtk.sh'),
    join(root, 'artifacts/plan11/baseline/boxrig_term'),
    join(root, 'build/tests/performance/annotation_gtk_peer'),
    join(root, 'build/tests/performance/annotation_gtk_probe'),
    'cairo', '10000', 'idle', 'none', directory], {
    cwd: root,
    env: { ...process.env, BOXRIG_ANNOTATION_URL_PERIOD: '10',
      BOXRIG_ANNOTATION_GTK_KEYS: '20' }, stdio: 'inherit',
  });
}
const terminalLog = readFileSync(join(directory, 'terminal.log'), 'utf8');
const probeLog = readFileSync(join(directory, 'probe.log'), 'utf8');
const excluded = [{
  frontend: 'baseline', renderer: 'cairo', history: 10000, load: 'idle',
  capture: 'none', period: 10, directory,
  reason: terminalLog.includes('GTK 3 application failed') ?
    'GTK3 sample terminated before key measurement; XGetImage subsequently received BadDrawable' :
    'stress trial failed before complete measurement',
  terminal_log: join(directory, 'terminal.log'),
  probe_log: join(directory, 'probe.log'),
  observed_probe_error: probeLog.trim(),
}];
writeFileSync(join(output, 'exclusions.json'), JSON.stringify(excluded, null, 2));
console.log(`recorded ${excluded.length} excluded stress trial`);
