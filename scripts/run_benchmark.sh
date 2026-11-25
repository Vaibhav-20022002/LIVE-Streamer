#!/usr/bin/env bash
set -euo pipefail

MAX_DURATION_SEC=12
HLS_SEGMENT_DURATION=2
PROFILING_LOG_INTERVAL_SEC=4

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

BUILD_DIR="./build"
BINARY="$BUILD_DIR/live_streamer"

echo "=========================================================="
echo " Starting Live Video Streaming Solution Benchmark Suite"
echo "=========================================================="

if [[ ! -x "$BINARY" ]]; then
    echo "[*] Binary $BINARY not found. Building via meson..."
    if [[ ! -d "$BUILD_DIR" ]]; then
        meson setup "$BUILD_DIR" --buildtype=debugoptimized
    fi
    meson compile -C "$BUILD_DIR"
fi

mkdir -p "docs/profiling"

BENCH_WORKSPACE=$(mktemp -d /tmp/streamer_bench.XXXXXX)
cleanup() {
    echo "[*] Cleaning up temporary benchmark workspace: $BENCH_WORKSPACE"
    rm -rf "$BENCH_WORKSPACE"
}
trap cleanup EXIT

echo "[*] Using temporary workspace: $BENCH_WORKSPACE"

run_scenario() {
    local mode_name="$1"
    shift
    local env_args=("$@")

    echo ""
    echo "----------------------------------------------------------"
    echo " Running Scenario: $mode_name"
    echo "----------------------------------------------------------"

    local scenario_dir="$BENCH_WORKSPACE/$mode_name"
    mkdir -p "$scenario_dir/hls"

    local time_cmd=""
    if [[ "$(uname)" == "Darwin" ]]; then
        time_cmd="/usr/bin/time -l"
    else
        time_cmd="/usr/bin/time -v"
    fi

    local log_file="$scenario_dir/run.log"

    set +e
    env "${env_args[@]}" \
        HLS_OUTPUT_DIR="$scenario_dir/hls" \
        MAX_DURATION_SEC="$MAX_DURATION_SEC" \
        HLS_SEGMENT_DURATION="$HLS_SEGMENT_DURATION" \
        PROFILING_LOG_INTERVAL_SEC="$PROFILING_LOG_INTERVAL_SEC" \
        $time_cmd "$BINARY" > "$log_file" 2>&1
    set -e
}

# 1. Baseline (Sync AI + Unpooled Allocations + Default Threads)
run_scenario "Baseline" \
    SOURCE_TYPE=lavfi \
    SOURCE_URL="testsrc2=size=1280x720:rate=30" \
    SOURCE_WIDTH=1280 \
    SOURCE_HEIGHT=720 \
    SOURCE_FPS=30 \
    TRANSCODE_MODE=always \
    AI_ENABLED=true \
    SYNTHETIC_DETECTIONS=true \
    AI_SYNC_MODE=true \
    USE_BUFFER_POOL=false \
    ENCODER_THREADS=0 \
    AI_THREADS=0

# 2. Optimized (Async Ring-Buffer AI + BufferPool + Explicit Thread Caps)
run_scenario "Optimized" \
    SOURCE_TYPE=lavfi \
    SOURCE_URL="testsrc2=size=1280x720:rate=30" \
    SOURCE_WIDTH=1280 \
    SOURCE_HEIGHT=720 \
    SOURCE_FPS=30 \
    TRANSCODE_MODE=always \
    AI_ENABLED=true \
    SYNTHETIC_DETECTIONS=true \
    AI_SYNC_MODE=false \
    USE_BUFFER_POOL=true \
    ENCODER_THREADS=2 \
    AI_THREADS=2

# 3. Smart Remux (Zero-Copy Pass-Through)
MP4_FILE="$BENCH_WORKSPACE/sample_input.mp4"
echo "[*] Generating synthetic 15s H.264 MP4 source for Smart Remux via ffmpeg..."
ffmpeg -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -t 15 -c:v libx264 -preset ultrafast -g 30 "$MP4_FILE" >/dev/null 2>&1

run_scenario "Smart_Remux" \
    SOURCE_TYPE=file \
    SOURCE_URL="$MP4_FILE" \
    TRANSCODE_MODE=auto \
    AI_ENABLED=false

echo ""
echo "=========================================================="
echo " Parsing Measured Results & Generating SVGs"
echo "=========================================================="

python3 - <<EOF
import json
import os
import re

bench_ws = "$BENCH_WORKSPACE"
scenarios = ["Baseline", "Optimized", "Smart_Remux"]
results = []

for sc in scenarios:
    sc_dir = os.path.join(bench_ws, sc)
    log_path = os.path.join(sc_dir, "run.log")
    stats_path = os.path.join(sc_dir, "hls", "stats.json")

    frames_processed = 0
    effective_fps = 0.0
    decode_avg = 0.0
    decode_p95 = 0.0
    inf_avg = 0.0
    inf_p95 = 0.0
    enc_avg = 0.0
    enc_p95 = 0.0
    total_avg = 0.0
    total_p95 = 0.0
    peak_rss = 0.0
    ai_inferences = 0

    if os.path.exists(log_path):
        with open(log_path, "r") as f:
            log_content = f.read()

        if os.uname().sysname == "Darwin":
            m = re.search(r"([0-9]+)\s+maximum resident set size", log_content)
            if m:
                peak_rss = round(int(m.group(1)) / (1024 * 1024), 2)
        else:
            m = re.search(r"Maximum resident set size \(kbytes\):\s+([0-9]+)", log_content)
            if m:
                peak_rss = round(int(m.group(1)) / 1024, 2)

        m_frames = re.findall(r"frames=([0-9]+)", log_content)
        if m_frames:
            frames_processed = int(m_frames[-1])

    if os.path.exists(stats_path):
        with open(stats_path, "r") as f:
            sdata = json.load(f)
        frames_processed = sdata.get("frames_processed", frames_processed)
        ai_inferences = sdata.get("ai_inferences", 0)
        effective_fps = sdata.get("output_fps", sdata.get("ingest_fps", 0.0))
        uptime = sdata.get("uptime_sec", 0.0)
        if effective_fps == 0.0 and uptime > 0.1 and frames_processed > 0:
            effective_fps = round(frames_processed / uptime, 2)
        stages = sdata.get("stages", {})
        dec = stages.get("decode", {})
        inf = stages.get("inference", {})
        enc = stages.get("encode", {})
        mux = stages.get("mux", {})
        tot = stages.get("total", {})
        decode_avg = dec.get("avg_ms", 0.0)
        decode_p95 = dec.get("p95_ms", 0.0)
        inf_avg = inf.get("avg_ms", 0.0)
        inf_p95 = inf.get("p95_ms", 0.0)
        enc_avg = enc.get("avg_ms", 0.0) if enc.get("count", 0) > 0 else mux.get("avg_ms", 0.0)
        enc_p95 = enc.get("p95_ms", 0.0) if enc.get("count", 0) > 0 else mux.get("p95_ms", 0.0)
        total_avg = tot.get("avg_ms", 0.0)
        total_p95 = tot.get("p95_ms", 0.0)

    if effective_fps == 0.0 and frames_processed > 0:
        effective_fps = round(frames_processed / 12.0, 2)

    results.append({
        "mode": sc.replace("_", " "),
        "frames_processed": frames_processed,
        "ai_inferences": ai_inferences,
        "effective_fps": round(effective_fps, 2),
        "decode_avg": round(decode_avg, 3),
        "decode_p95": round(decode_p95, 3),
        "inference_avg": round(inf_avg, 3),
        "inference_p95": round(inf_p95, 3),
        "encode_avg": round(enc_avg, 3),
        "encode_p95": round(enc_p95, 3),
        "total_avg": round(total_avg, 3),
        "total_p95": round(total_p95, 3),
        "peak_rss_mb": peak_rss
    })

os.makedirs("docs/profiling", exist_ok=True)
output_json = "docs/profiling/latest_results.json"
with open(output_json, "w") as f:
    json.dump(results, f, indent=2)

print(f"[*] Wrote JSON summary to {output_json}")
print("")
print("| Mode | Frames | Eff. FPS | Decode avg/p95 (ms) | AI Inf avg/p95 (ms) | Encode/Mux avg/p95 (ms) | Total Frame avg/p95 (ms) | Peak RSS (MB) |")
print("|---|---:|---:|---:|---:|---:|---:|---:|")
for r in results:
    print(f"| {r['mode']} | {r['frames_processed']} | {r['effective_fps']:.2f} | "
          f"{r['decode_avg']:.3f} / {r['decode_p95']:.3f} | "
          f"{r['inference_avg']:.3f} / {r['inference_p95']:.3f} | "
          f"{r['encode_avg']:.3f} / {r['encode_p95']:.3f} | "
          f"{r['total_avg']:.3f} / {r['total_p95']:.3f} | {r['peak_rss_mb']:.2f} |")

max_val = max([r["total_p95"] for r in results] + [1.0])
scale_max = max(max_val * 1.25, 2.0)
chart_top = 80
chart_bottom = 320
chart_h = chart_bottom - chart_top
colors = ["#ef4444", "#38bdf8", "#10b981"]

bars_svg = []
for idx, r in enumerate(results):
    x_avg = 150 + idx * 190
    x_p95 = x_avg + 56
    h_avg = max(2.0, (r["total_avg"] / scale_max) * chart_h)
    h_p95 = max(2.0, (r["total_p95"] / scale_max) * chart_h)
    y_avg = chart_bottom - h_avg
    y_p95 = chart_bottom - h_p95
    col = colors[idx % len(colors)]
    bars_svg.append(
        f'  <rect x="{x_avg}" y="{y_avg:.1f}" width="48" height="{h_avg:.1f}" fill="{col}" rx="4"/>\n'
        f'  <text x="{x_avg + 24}" y="{y_avg - 6:.1f}" text-anchor="middle" class="val">{r["total_avg"]:.2f}ms</text>\n'
        f'  <rect x="{x_p95}" y="{y_p95:.1f}" width="48" height="{h_p95:.1f}" fill="{col}" fill-opacity="0.55" rx="4"/>\n'
        f'  <text x="{x_p95 + 24}" y="{y_p95 - 6:.1f}" text-anchor="middle" class="val">{r["total_p95"]:.2f}ms</text>\n'
        f'  <text x="{x_avg + 52}" y="344" text-anchor="middle" class="label">{r["mode"]}</text>\n'
        f'  <text x="{x_avg + 52}" y="362" text-anchor="middle" class="sub">({r["effective_fps"]:.1f} fps | {r["peak_rss_mb"]:.1f} MB)</text>'
    )

grid_svg = []
for frac in [0.25, 0.5, 0.75, 1.0]:
    yv = chart_bottom - frac * chart_h
    lbl = scale_max * frac
    grid_svg.append(
        f'  <line x1="100" y1="{yv:.1f}" x2="740" y2="{yv:.1f}" class="grid"/>\n'
        f'  <text x="90" y="{yv + 4:.1f}" text-anchor="end" class="label">{lbl:.1f}ms</text>'
    )

svg_content = f'''<svg width="780" height="400" xmlns="http://www.w3.org/2000/svg">
  <style>
    .title {{ font: bold 17px sans-serif; fill: #f8fafc; }}
    .subtitle {{ font: 12px sans-serif; fill: #94a3b8; }}
    .label {{ font: bold 12px sans-serif; fill: #cbd5e1; }}
    .sub {{ font: 11px sans-serif; fill: #94a3b8; }}
    .val {{ font: bold 11px monospace; fill: #f1f5f9; }}
    .axis {{ stroke: #475569; stroke-width: 1.5; }}
    .grid {{ stroke: #1e293b; stroke-width: 1; stroke-dasharray: 4; }}
  </style>
  <rect width="100%" height="100%" fill="#0f172a" rx="8"/>
  <text x="390" y="32" text-anchor="middle" class="title">Per-Frame Processing Latency: Avg (solid) vs P95 (shaded)</text>
  <text x="390" y="52" text-anchor="middle" class="subtitle">1280x720 @ 30fps, 12s benchmark (lower is faster)</text>
{chr(10).join(grid_svg)}
  <line x1="100" y1="320" x2="740" y2="320" class="axis"/>
  <line x1="100" y1="80" x2="100" y2="320" class="axis"/>
{chr(10).join(bars_svg)}
</svg>'''

with open("docs/profiling/latency_comparison.svg", "w") as f:
    f.write(svg_content)

print("[*] Wrote docs/profiling/latency_comparison.svg")

baseline_r = next((r for r in results if r["mode"] == "Baseline"), results[0])
opt_r = next((r for r in results if r["mode"] == "Optimized"), results[0])
remux_r = next((r for r in results if r["mode"] == "Smart Remux"), results[-1])

def stage_bar(y, title, r, total_budget=33.33):
    bar_w = 500.0
    scale = bar_w / total_budget
    segs = [
        ("Decode", r["decode_avg"], "#38bdf8"),
        ("AI Sync" if r["mode"] == "Baseline" else "AI Async", r["inference_avg"] if r["mode"] == "Baseline" else 0.0, "#f59e0b"),
        ("Encode/Mux", r["encode_avg"], "#10b981"),
    ]
    out = [f'<text x="20" y="{y + 18}" class="label">{title}</text>']
    x = 165.0
    for name, ms, col in segs:
        w = max(0.0, ms * scale)
        if w >= 1.5:
            out.append(f'<rect x="{x:.1f}" y="{y}" width="{min(w, 690 - x):.1f}" height="26" fill="{col}" rx="3"/>')
            if w > 42:
                out.append(f'<text x="{x + min(w, 690 - x)/2:.1f}" y="{y + 17}" text-anchor="middle" class="seg">{name} {ms:.2f}ms</text>')
            x += w
    out.append(f'<text x="760" y="{y + 18}" text-anchor="end" class="val">Total {r["total_avg"]:.2f}ms (p95 {r["total_p95"]:.2f}ms)</text>')
    return "\n  ".join(out)

flame_svg = f'''<svg width="780" height="255" xmlns="http://www.w3.org/2000/svg">
  <style>
    .title {{ font: bold 16px sans-serif; fill: #f8fafc; }}
    .subtitle {{ font: 12px sans-serif; fill: #94a3b8; }}
    .label {{ font: bold 12px sans-serif; fill: #e2e8f0; }}
    .seg {{ font: bold 10px sans-serif; fill: #0f172a; }}
    .val {{ font: 11px monospace; fill: #cbd5e1; }}
  </style>
  <rect width="100%" height="100%" fill="#0f172a" rx="8"/>
  <text x="390" y="30" text-anchor="middle" class="title">Pipeline Stage Span Breakdown vs 33.3ms (30 FPS) Frame Budget</text>
  <text x="390" y="50" text-anchor="middle" class="subtitle">Critical-path wall time per frame (avg ms)</text>
  {stage_bar(80, "Baseline (Sync)", baseline_r)}
  {stage_bar(130, "Optimized (Async)", opt_r)}
  {stage_bar(180, "Smart Remux", remux_r)}
  <text x="20" y="234" class="subtitle">Note: In Optimized mode, AI inference runs asynchronously off the critical frame path via DropOldestRingBuffer.</text>
</svg>'''

with open("docs/profiling/pipeline_flamegraph.svg", "w") as f:
    f.write(flame_svg)

print("[*] Wrote docs/profiling/pipeline_flamegraph.svg")
EOF

echo "=========================================================="
echo " Benchmark Complete!"
echo "=========================================================="
