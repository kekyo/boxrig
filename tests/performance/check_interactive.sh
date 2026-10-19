#!/usr/bin/env bash
set -euo pipefail
node --input-type=module - "$1" <<'JS'
import {readFileSync, writeFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
const directory = process.argv[2];
const parse = name => readFileSync(`${directory}/${name}`, 'utf8').trim().split('\n');
const metadata = line => Object.fromEntries([...line.matchAll(/(\w+)=(\d+)/g)].map(m => [m[1], +m[2]]));
const eventLines = parse('events.tsv'), childLines = parse('child.tsv');
const view = metadata(eventLines[0]), load = metadata(childLines[0]);
const events = eventLines.slice(2).map(line => line.split('\t').map(Number));
const child = childLines.slice(2).map(line => line.split('\t').map(Number));
const frames = parse('frames.tsv').slice(1).map(line => line.split('\t').map(Number));
const operations = view.record ? parse('operations.tsv').slice(1).map(line => line.split('\t')) : [];
const check = (value, message) => { if (!value) throw Error(message); };
const distribution = values => {
    if (!values.length) return null;
    const ordered = [...values].sort((a,b) => a-b);
    return {n:values.length, min:ordered[0], median:ordered[Math.floor(ordered.length/2)],
        p95:ordered[Math.ceil(ordered.length*0.95)-1], max:ordered.at(-1), values};
};
check(events.length === 20 && child.length === 20, 'missing key events');
check(operations.length === (view.record ? 20 : 0), 'missing wheel/selection operations');
const received = [], visible = [];
for (let i=0; i<20; i++) {
    check(events[i][0] === i+1 && child[i][0] === i+1, 'key order');
    check(events[i][1] <= child[i][1] && child[i][1] <= child[i][2], 'key causal order');
    received.push((child[i][1]-events[i][1])/1e6);
    if (view.record) {
        check(events[i][2] >= child[i][1], 'visible key precedes reception');
        check(frames.some(frame => frame[2] === events[i][2]), 'visible timestamp lacks a frame');
        visible.push((events[i][2]-events[i][1])/1e6);
    } else check(events[i][2] === 0, 'unexpected visible result without capture');
}
const capture = [], periods = [], updates = [], keyUpdates = [];
let previousUpdate = null, duplicates = 0, missed = 0;
if (view.record) {
    const pts = parse('video-timestamps.txt').map(Number);
    check(pts.length === frames.length, 'video frame count');
    const size = view.cw*80*view.ch*24*3;
    const raw = execFileSync('ffmpeg', ['-nostdin','-v','error','-i',`${directory}/video.mkv`,
        '-fps_mode','passthrough','-enc_time_base','demux',
        '-f','rawvideo','-pix_fmt','rgb24','pipe:1'], {maxBuffer:1024*1024*1024});
    check(raw.length === size*frames.length, 'decoded video dimensions');
    const marker = (Math.floor(view.ch/2)*view.cw*80+view.cw*3)*3;
    let keyIndex = 0;
    for (let i=0; i<frames.length; i++) {
        const [index,start,end,dropped,source] = frames[i];
        check(index === i && end >= start && source <= i, 'invalid frame metadata');
        check(Math.abs(pts[i]-(end-frames[0][2])/1e9) <= 0.0011, 'video time differs from acquisition');
        if (i) {
            check(start >= frames[i-1][2], 'overlapping frame acquisitions');
            periods.push((end-frames[i-1][2])/1e6);
        }
        capture.push((end-start)/1e6);
        missed += dropped;
        while (keyIndex<20 && events[keyIndex][2] <= end) keyIndex++;
        const color = keyIndex === 0 ? [32,64,96] : [64+keyIndex*5,160,80];
        if (end < view.key_phase_end_ns) {
            check(color.every((value,channel) => raw[i*size+marker+channel] === value),
                `video key marker mismatch at frame ${i}`);
        }
        const pixel = (x,y) => {
            const p=i*size+(y*view.cw*80+x)*3;
            return raw[p]*65536+raw[p+1]*256+raw[p+2];
        };
        let row=-1, mask=0;
        for(let y=0;y<24;y++) if(pixel(3*view.cw,y*view.ch+Math.floor(view.ch/2))===0xa4a050){row=y;break;}
        for(let x=8;x<24;x++) {
            const offset=((view.ch-1)*view.cw*80+(x+1)*view.cw-1)*3;
            const baseline=raw[offset]*65536+raw[offset+1]*256+raw[offset+2];
            if(pixel((x+1)*view.cw-1,view.ch-1)!==baseline) mask|=1<<(x-8);
        }
        check(row === frames[i][9] && mask === frames[i][10], `operation pixels differ at frame ${i}`);
        const duplicate = i !== 0 && raw.subarray(i*size,(i+1)*size).equals(raw.subarray((i-1)*size,i*size));
        check(duplicate === (source !== i), 'duplicate classification differs from visible pixels');
        if (duplicate) duplicates++;
        else {
            if (previousUpdate !== null) {
                updates.push((end-previousUpdate)/1e6);
                if (end < view.key_phase_end_ns) keyUpdates.push((end-previousUpdate)/1e6);
            }
            previousUpdate = end;
        }
    }
} else check(frames.length === 0, 'unexpected frames');
const operationTimes = {up:[],down:[],select:[],clear:[],primary:[]};
const scrollRows = [];
for(let i=0;i<operations.length;i++) {
    const [index,kind,start,end,copied,column,mask,attempts,text] = operations[i];
    check(+index===i && +end>=+start && +start>=view.key_phase_end_ns, 'operation causal order');
    const expectedKind = i<10 ? (i%2===0?'up':'down') : (i%2===0?'select':'clear');
    check(kind === expectedKind, 'operation sequence');
    if(i) check(+start>=Math.max(+operations[i-1][3],+operations[i-1][4])+250e6, 'operation spacing');
    const frame = frames.find(f=>f[2]===+end);
    check(frame, 'operation has no endpoint frame');
    if(kind==='up') {check(frame[9]>0, 'wheel did not move into history');scrollRows.push(frame[9]);}
    else check(frame[9]===0, 'operation is not on live fixed row');
    if(kind==='select') {
        check(frame[10]===+mask && +mask===(+column===8?65535:65408), 'selected range differs');
        check(text===(+column===8?'SELECT_THIS_TEXT':'THIS_TEXT'), 'selected text differs');
        check(+copied>=+start && +attempts>0, 'missing PRIMARY response');
        operationTimes.primary.push((+copied-start)/1e6);
    } else {
        check(+copied===0 && +attempts===0, 'unexpected PRIMARY request');
        if(kind==='clear') check(frame[10]===0, 'selection remained visible');
    }
    operationTimes[kind].push((end-start)/1e6);
}
check(load.rows === 24 && load.columns === 80 && load.history_lines === 10024, 'grid/history mismatch');
const result = {view,load,received_ms:distribution(received),visible_ms:distribution(visible),
    capture_ms:distribution(capture),capture_period_ms:distribution(periods),
    observed_update_ms:distribution(updates),frame_count:frames.length,duplicates,missed,
    key_observed_update_ms:distribution(keyUpdates),scroll_rows:scrollRows,
    operations_ms:Object.fromEntries(Object.entries(operationTimes).map(([kind,times])=>[kind,distribution(times)])),
    observer_cpu_percent:view.cpu_ns/(view.end_ns-view.start_ns)*100,
    output_bytes_per_second:load.output_bytes/((load.ended_ns-load.started_ns)/1e9)};
writeFileSync(`${directory}/summary.json`,JSON.stringify(result,null,2));
console.log(`PASS interactive recording: ${directory}, ${frames.length} frames, 20 keys`);
JS
