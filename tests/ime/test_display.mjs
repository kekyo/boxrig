/* boxrig - Under MIT. Observe real daemon input and its displayed candidates. */
import assert from 'node:assert/strict';
import {spawn, execFileSync} from 'node:child_process';
import {once, EventEmitter} from 'node:events';
import {createConnection, createServer} from 'node:net';
import {createInterface} from 'node:readline';
import {mkdtemp, mkdir, readFile, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';

if (process.argv[2] === '--child') {
    process.stdin.setRawMode(true);
    process.stdin.resume();
    const socket = createConnection(process.argv[3]);
    await once(socket, 'connect');
    process.stdout.write('\x1b]2;boxrig-fcitx-display\x07\x1b[2J\x1b[5;8H');
    socket.write(JSON.stringify({ready: true}) + '\n');
    process.stdin.on('data', bytes => socket.write(JSON.stringify({input: bytes.toString()}) + '\n'));
    for await (const line of createInterface({input: socket})) {
        assert.equal(line, 'quit');
        process.stdin.pause();
        socket.end();
        break;
    }
} else {
    const [samplePath, backend, renderer, destination] = process.argv.slice(2);
    assert(samplePath && ['wayland', 'gtk3'].includes(backend) && renderer && destination);
    const output = resolve(destination);
    await mkdir(output, {recursive: true});
    const temporary = await mkdtemp(join(tmpdir(), 'boxrig-fcitx-display-'));
    const socketPath = join(temporary, 'child.sock');
    const events = new EventEmitter();
    const server = createServer();
    let socket, sample, recorder, recorderClosed, diagnostics = '', errors = '';
    let state = {}, ready = false, received = '', closed = false;
    const command = (name, args, extra = {}) => execFileSync(name, args,
        {encoding: 'utf8', timeout: 60000, ...extra}).trim();
    const xdo = (...args) => command('xdotool', args);
    const awaitState = async predicate => {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), 60000);
        try {
            while (!predicate()) {
                assert(!closed, 'Sample exited early: ' + errors);
                await once(events, 'changed', {signal: controller.signal});
            }
        } finally { clearTimeout(timer); }
    };
    const capture = async name => {
        const pixels = execFileSync('ffmpeg', ['-nostdin', '-v', 'error', '-f', 'x11grab',
            '-draw_mouse', '0', '-video_size', '1280x800', '-i', process.env.DISPLAY,
            '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'],
            {maxBuffer: 1280 * 800 * 4, timeout: 60000});
        assert.equal(pixels.length, 1280 * 800 * 3);
        execFileSync('ffmpeg', ['-nostdin', '-v', 'error', '-f', 'rawvideo', '-pixel_format', 'rgb24',
            '-video_size', '1280x800', '-i', 'pipe:0', '-frames:v', '1', '-y', join(output, name + '.png')],
            {input: pixels, timeout: 60000});
        return pixels;
    };
    try {
        server.on('connection', connection => {
            socket = connection;
            createInterface({input: connection}).on('line', line => {
                const message = JSON.parse(line);
                if (message.ready) ready = true;
                if (message.input) received += message.input;
                events.emit('changed');
            });
        });
        server.listen(socketPath);
        await once(server, 'listening');
        sample = spawn(resolve(samplePath), ['--renderer=' + renderer, '--', process.execPath,
            import.meta.filename, '--child', socketPath], {env: {...process.env,
            BOXRIG_WAYLAND_IME: 'auto', BOXRIG_GTK_IME: 'fcitx5',
            BOXRIG_IME_CANDIDATES: backend === 'wayland' ? 'client' : 'system'},
            stdio: ['ignore', 'pipe', 'pipe']});
        sample.on('close', () => { closed = true; events.emit('changed'); });
        sample.stderr.on('data', bytes => { errors += bytes; });
        createInterface({input: sample.stdout}).on('line', line => {
            diagnostics += line + '\n';
            if (line.includes('ime-route=')) {
                state = Object.fromEntries([...line.matchAll(/([\w-]+)=([^ ]+)/g)].map(match => [match[1], match[2]]));
                events.emit('changed');
            }
        });
        await awaitState(() => ready && state['ime-route'] === 'fcitx5' && state['direct-state'] === '2');
        let window, bounds;
        if (backend === 'wayland') {
            const log = await readFile(process.env.BOXRIG_TEST_WESTON_LOG, 'utf8');
            window = /window id (\d+)/.exec(log)?.[1];
            assert(window, 'Weston X11 output window');
            const scene = command('weston-debug', ['scene-graph']);
            const section = scene.split(/(?=View \d+ \(role )/).find(part =>
                part.includes('PID ' + sample.pid + ',') && part.includes('role xdg_toplevel'));
            const rectangle = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(section ?? '');
            assert(rectangle, scene);
            bounds = rectangle.slice(1).map(Number);
            xdo('windowfocus', '--sync', window);
            xdo('mousemove', '--window', window, String(bounds[0] + 120), String(bounds[1] + 100), 'click', '1');
        } else {
            window = xdo('search', '--sync', '--onlyvisible', '--pid', String(sample.pid),
                '--name', '^boxrig-fcitx-display$').split('\n')[0];
            const geometry = Object.fromEntries(xdo('getwindowgeometry', '--shell', window)
                .split('\n').map(line => line.split('=')));
            bounds = [Number(geometry.X), Number(geometry.Y), Number(geometry.X) + Number(geometry.WIDTH),
                Number(geometry.Y) + Number(geometry.HEIGHT)];
            xdo('windowfocus', '--sync', window);
            xdo('mousemove', '--window', window, '120', '100', 'click', '1');
        }
        recorder = spawn('ffmpeg', ['-nostdin', '-v', 'error', '-f', 'x11grab', '-framerate', '15',
            '-draw_mouse', '0', '-video_size', '1280x800', '-i', process.env.DISPLAY,
            '-c:v', 'ffv1', '-progress', 'pipe:1', '-y', join(output, 'candidates.mkv')],
            {stdio: ['ignore', 'pipe', 'pipe']});
        recorderClosed = once(recorder, 'close');
        let recording = false;
        createInterface({input: recorder.stdout}).on('line', line => {
            if (/^frame=\s*[1-9]/.test(line)) { recording = true; events.emit('changed'); }
        });
        await awaitState(() => recording);
        command('gdbus', ['call', '--session', '--dest', 'org.fcitx.Fcitx5', '--object-path', '/controller',
            '--method', 'org.fcitx.Fcitx.Controller1.SetCurrentIM', 'mozc']);
        const before = await capture('before');
        xdo('type', '--clearmodifiers', '--delay', '0', 'kisha');
        xdo('key', '--clearmodifiers', 'space', 'space');
        let popup;
        if (backend === 'wayland') {
            await awaitState(() => state['candidate-owner'] === 'client' && Number(state['candidate-count']) > 0 &&
                state['candidate-visible'] === '1');
        } else {
            popup = xdo('search', '--sync', '--onlyvisible', '--name', '^Fcitx5 Input Window$').split('\n')[0];
            assert(popup);
            assert.equal(state['candidate-owner'], 'system');
        }
        const page = await capture('page');
        let changed = 0, first = 800, last = -1;
        for (let y = Math.max(0, bounds[1] + 35); y < Math.min(800, bounds[3] - 20); y++)
        for (let x = Math.max(0, bounds[0] + 10); x < Math.min(1280, bounds[2] - 20); x++) {
            const at = (y * 1280 + x) * 3;
            if (Math.abs(page[at] - before[at]) + Math.abs(page[at + 1] - before[at + 1]) +
                Math.abs(page[at + 2] - before[at + 2]) > 90) {
                changed++; first = Math.min(first, y); last = Math.max(last, y);
            }
        }
        assert(changed > 300 && last - first > 40, 'Candidate rows must be painted inside the terminal area');
        assert.equal(received, '', 'Composition must not leak romanization into the PTY');
        const revision = Number(state['candidate-revision']);
        xdo('key', '--clearmodifiers', 'Page_Down');
        if (backend === 'wayland') await awaitState(() => Number(state['candidate-revision']) > revision);
        const next = await capture('next-page');
        assert(!page.equals(next), 'Paging must update the displayed candidate content');
        xdo('key', '--clearmodifiers', 'Return');
        await awaitState(() => received.length > 0);
        assert(/\p{Script=Han}/u.test(received) && !/[a-z\r\n]/.test(received), received);
        const committed = received;
        if (backend === 'wayland') await awaitState(() => state['candidate-visible'] === '0');
        await capture('committed');
        xdo('type', '--clearmodifiers', '--delay', '0', 'kisha');
        xdo('key', '--clearmodifiers', 'space', 'space');
        if (backend === 'wayland') await awaitState(() => state['candidate-visible'] === '1');
        /* Mozc first returns from conversion to its reading, then cancels
         * that reading. Suggestions may still be visible after one Escape. */
        xdo('key', '--clearmodifiers', 'Escape', 'Escape');
        if (backend === 'wayland') await awaitState(() => state['candidate-visible'] === '0');
        await capture('cancelled');
        assert.equal(received, committed, 'Cancellation must not emit text');
        recorder.kill('SIGINT');
        const [videoCode] = await recorderClosed;
        assert(videoCode === 0 || videoCode === 255, 'Recording must finish cleanly');
        const cropX = Math.max(0, bounds[0] + 10), cropY = Math.max(0, bounds[1] + 35);
        const cropWidth = Math.min(1280, bounds[2] - 20) - cropX;
        const cropHeight = Math.min(800, bounds[3] - 20) - cropY;
        const frames = command('ffmpeg', ['-nostdin', '-v', 'error', '-i', join(output, 'candidates.mkv'),
            '-vf', `crop=${cropWidth}:${cropHeight}:${cropX}:${cropY}`, '-f', 'framemd5', 'pipe:1'])
            .split('\n').filter(line => !line.startsWith('#') && line.trim()).map(line => line.split(',').at(-1).trim());
        assert(frames.length >= 5 && new Set(frames).size >= 3,
            'The lossless recording must contain changing terminal frames across the observed input phases');
        await writeFile(join(output, 'observations.json'), JSON.stringify({backend, renderer,
            bounds, candidatePixels: changed, videoFrames: frames.length, committed,
            phases: ['before', 'page', 'next-page', 'committed', 'cancelled']}, null, 2));
        socket.end('quit\n');
        const [code, signal] = await once(sample, 'close');
        assert.equal(signal, null, errors); assert.equal(code, 0, errors);
        console.log(`PASS real fcitx5 ${backend}/${renderer}: visible candidates, paging, one commit, cancellation`);
    } finally {
        if (recorder && recorder.exitCode === null) recorder.kill('SIGINT');
        if (recorderClosed) await recorderClosed;
        if (sample && !closed) { sample.kill('SIGTERM'); await once(sample, 'close'); }
        socket?.destroy(); server.close();
        await writeFile(join(output, 'sample.log'), diagnostics + errors);
        await rm(temporary, {recursive: true, force: true});
    }
}
