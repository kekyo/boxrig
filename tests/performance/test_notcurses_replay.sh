#!/bin/bash
# Replay fixtures do not require notcurses, a display server, or a recording.
set -euo pipefail
replay=$(realpath "$1")
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
printf '\033[45;118HZ\033[6n\033[14t\033[16t\033[18t' > "$out/raw"
"$replay" "$out/raw" 45 118 7 14 raw > "$out/raw.json"
jq -e '.cursor_row == 44 and .cursor_column == 117 and
    .reply_prefix == "1b5b34353b313138521b5b343b3633303b383236741b5b363b31343b3774"' "$out/raw.json"
{
    printf 'Script started on fixture [COMMAND="demo" <not executed on terminal>]\n'
    cat "$out/raw"
    printf '\nScript done on fixture [COMMAND_EXIT_CODE="0"]\n'
} > "$out/script"
"$replay" "$out/script" 45 118 7 14 script > "$out/script.json"
diff <(jq 'del(.seconds,.cpu_seconds,.input_format)' "$out/raw.json") \
    <(jq 'del(.seconds,.cpu_seconds,.input_format)' "$out/script.json")
printf 'Script started on fixture\nunterminated' > "$out/incomplete"
if "$replay" "$out/incomplete" 45 118 7 14 script; then exit 1; fi
if "$replay" "$out/raw" 0 118 7 14 raw; then exit 1; fi
if "$replay" "$out/raw" 45 118x 7 14 raw; then exit 1; fi
sed 's/COMMAND_EXIT_CODE="0"/COMMAND_EXIT_CODE="1"/' "$out/script" > "$out/failed"
if "$replay" "$out/failed" 45 118 7 14 script; then exit 1; fi
# A tiny opaque red image exercises decoding, placement, and deletion.
printf '\033_Ga=T,f=32,s=1,v=1,i=17,p=2;/wAA/w==\033\\' > "$out/image"
"$replay" "$out/image" 45 118 7 14 raw > "$out/image.json"
jq -e '.peak_images == 1 and .peak_placements == 1 and
    .image_rgba_prefix == "ff0000ff"' "$out/image.json"
printf '\033_Ga=d,d=A\033\\' >> "$out/image"
"$replay" "$out/image" 45 118 7 14 raw > "$out/deleted.json"
jq -e '.final_images == 0 and .final_placements == 0' "$out/deleted.json"
echo 'PASS replay geometry, terminal replies, script framing, image pixels and deletion'
