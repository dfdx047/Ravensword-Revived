#!/usr/bin/env bash
# Real frame pacing of the game's surface over N seconds (default 8), from SurfaceFlinger.
A=(adb)  # set ANDROID_SERIAL to pick a device
L=$("${A[@]}" shell dumpsys SurfaceFlinger --list | grep -m1 -oE 'SurfaceView\[com\.crescentmoongames\.ravensword2[^]]*\]\(BLAST\)#[0-9]+')
"${A[@]}" shell dumpsys SurfaceFlinger --latency-clear >/dev/null
sleep "${1:-8}"
"${A[@]}" shell "dumpsys SurfaceFlinger --latency '$L'" | python3 -I -c '
import sys
rows=[l.split() for l in sys.stdin if l.strip()]
ts=sorted(int(r[1]) for r in rows[1:] if len(r)==3 and 0<int(r[1])<(1<<62))
d=sorted((b-a)/1e6 for a,b in zip(ts,ts[1:]))
if len(d)<2: print("no frames captured", len(rows)); sys.exit()
print(f"frames={len(ts)} avg_fps={1000*len(d)/sum(d):.1f} median_ms={d[len(d)//2]:.2f} p99_ms={d[int(len(d)*.99)]:.2f} max_ms={d[-1]:.1f}")'
