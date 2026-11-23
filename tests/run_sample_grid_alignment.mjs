/* boxrig - Under MIT. */
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {once} from 'node:events';
import {existsSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';

const [frontend, binary, renderer] = process.argv.slice(2);
if (frontend === '--pty') {
    const paint = () => {
        const [columns, rows] = process.stdout.getWindowSize();
        process.stdout.write('\x1b[?25l\x1b[0m\x1b[2J\x1b[H\x1b[48;2;226;34;83m\x1b[2K' +
            '\x1b[' + rows + ';1H\x1b[48;2;22;206;101m\x1b[2K\x1b[0m\x1b]2;boxrig-grid-bands\x07');
        writeFileSync(binary + '.tmp', JSON.stringify({columns, rows}));
        renameSync(binary + '.tmp', binary);
    };
    process.stdout.on('resize', paint);
    process.stdin.resume();
    paint();
} else {
    const width = 653, height = 419;
    const directory = mkdtempSync(join(tmpdir(), 'boxrig-grid-placement-'));
    const env = {...process.env, LIBGL_ALWAYS_SOFTWARE: '1', GTK_A11Y: 'none',
        GTK_IM_MODULE: 'gtk-im-context-simple', GSK_RENDERER: 'cairo', GIO_USE_VFS: 'local',
        GTK_USE_PORTAL: '0', DBUS_SESSION_BUS_ADDRESS: 'unix:path=/dev/null',
        BOXRIG_SCROLLBAR: 'never', BOXRIG_WAYLAND_DECORATION: 'none'};
    const onWayland = frontend !== 'x11';
    const stop = async child => {
        if (!child || child.exitCode !== null || child.signalCode !== null) return;
        const closed = once(child, 'close');
        child.kill('SIGTERM');
        await closed;
    };
    const until = async (description, check) => {
        const deadline = Date.now() + 30000;
        do {
            const result = check();
            if (result) return result;
            await delay(40);
        } while (Date.now() < deadline);
        throw Error(description);
    };
    let weston;
    try {
        let output;
        if (onWayland) {
            env.XDG_RUNTIME_DIR = directory;
            env.WAYLAND_DISPLAY = 'grid-test';
            env.GDK_BACKEND = 'wayland';
            const log = join(directory, 'weston.log');
            weston = spawn('weston', ['--backend=x11', '--shell=kiosk-shell.so', '--renderer=gl',
                '--socket=grid-test', '--no-config', '--idle-time=0', '--width='+width, '--height='+height,
                '--log='+log], {env, stdio: 'ignore'});
            output = await until('Weston kiosk output was not mapped', () => {
                assert.equal(weston.exitCode, null);
                return existsSync(join(directory, env.WAYLAND_DISPLAY)) && existsSync(log) &&
                    /window id (\d+)/.exec(readFileSync(log, 'utf8'))?.[1];
            });
        }
        for (const alignment of ['top', 'bottom']) {
            const mark = join(directory, alignment);
            const app = spawn(resolve(binary), ['--renderer='+renderer, '--', process.execPath,
                fileURLToPath(import.meta.url), '--pty', mark], {env: {...env, BOXRIG_GRID_ALIGNMENT: alignment},
                stdio: ['ignore', 'ignore', 'inherit']});
            try {
                let window = output;
                if (!onWayland) {
                    window = await until('X11 terminal was not mapped', () => {
                        assert.equal(app.exitCode, null);
                        return spawnSync('xdotool', ['search', '--onlyvisible', '--name', '^boxrig-grid-bands$'],
                            {env, encoding:'utf8'}).stdout.trim();
                    });
                    assert.equal(spawnSync('xdotool', ['windowsize', window, String(width), String(height)], {env}).status, 0);
                }
                const bands = await until('terminal did not paint the resized PTY', () => {
                    assert.equal(app.exitCode, null);
                    if (!existsSync(mark)) return;
                    const {rows} = JSON.parse(readFileSync(mark, 'utf8'));
                    const frame = spawnSync('ffmpeg', ['-v','error','-f','x11grab','-framerate','60',
                        '-window_id',window,'-video_size',width+'x'+height,'-i',env.DISPLAY,
                        '-frames:v','1','-threads','1','-pix_fmt','rgb24','-f','rawvideo','pipe:1'],
                        {env, maxBuffer: width * height * 4});
                    assert.equal(frame.status, 0, frame.stderr.toString());
                    const red = [], green = [];
                    for (let y = 0; y < height; y++) {
                        const i = 3 * (width * y + 100);
                        const [r,g,b] = frame.stdout.subarray(i,i+3);
                        if (r === 226 && g === 34 && b === 83) red.push(y);
                        if (r === 22 && g === 206 && b === 101) green.push(y);
                    }
                    if (red.length && red.length === green.length && rows === Math.floor(height/red.length) &&
                        green.at(-1) - red[0] + 1 === rows * red.length)
                        return {first:red[0], last:green.at(-1), cell:red.length};
                });
                const spare = height % bands.cell;
                assert(spare > 0);
                assert.equal(bands.first, alignment === 'bottom' ? spare : 0, frontend+' '+renderer+' '+alignment+' first row');
                assert.equal(bands.last, height - 1 - (alignment === 'top' ? spare : 0), frontend+' '+renderer+' '+alignment+' last row');
                if (frontend === 'x11' && alignment === 'bottom') {
                    const hints = spawnSync('xprop', ['-id',window,'WM_NORMAL_HINTS'], {env,encoding:'utf8'});
                    assert.equal(hints.status, 0);
                    assert.doesNotMatch(hints.stdout, /resize increment: (?!1 by 1\b)/,
                        'window manager must be able to assign the whole tile');
                }
                console.log('PASS', frontend, renderer, alignment, bands);
            } finally { await stop(app); }
        }
    } finally {
        await stop(weston);
        rmSync(directory, {recursive:true, force:true});
    }
}
