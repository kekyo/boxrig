#!/usr/bin/env bash
set -euo pipefail
binary=$(realpath "$1")
output=$(mktemp -d "$2/input-profile-test.XXXXXXXX")
"$binary" input-ascii 24 80 4 64 8 1 > "$output/default.log"
"$binary" input-ascii 24 80 4 64 8 2 > "$output/image.log"
BOXRIG_PROFILE_MONITOR=1 "$binary" input-ascii 24 80 4 64 8 1 > "$output/monitor.log"
node --input-type=module - "$output" <<'JS'
import { readFileSync } from 'node:fs';
const directory = process.argv[2];
for (const condition of ['default', 'image', 'monitor']) {
    const text = readFileSync(`${directory}/${condition}.log`, 'utf8');
    const profile = text.split('\n').find(line => line.startsWith('PROFILE input '));
    if (!profile) throw Error('missing input profile');
    const v = Object.fromEntries([...profile.matchAll(/(\w+)=(\d+)/g)].map(m => [m[1], +m[2]]));
    if (v.instrumented !== 1 || v.batch_bytes + v.scalars !== 496 || v.decoded_bytes !== v.scalars) {
        throw Error(`invalid printed-byte counts: ${condition}`);
    }
    if (condition === 'default' && (v.batch_bytes === 0 || v.fallback_graphics || v.fallback_monitor)) {
        throw Error('default ASCII path was not measured');
    }
    if (condition !== 'default' && (v.batch_bytes || v[`fallback_${condition === 'image' ? 'graphics' : 'monitor'}`] !== 496)) {
        throw Error(`fallback path was not measured: ${condition}`);
    }
}
console.log('PASS input profile: batch coverage and graphics/monitor fallbacks');
JS
