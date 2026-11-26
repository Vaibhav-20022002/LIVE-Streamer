#!/usr/bin/env bash
set -euo pipefail

if [ -d "/Applications/Docker.app/Contents/Resources/bin" ]; then
    export PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi

MAX_DURATION_SEC=12
HLS_SEGMENT_DURATION=2
PROFILING_LOG_INTERVAL_SEC=4
DOCKER_IMAGE="livevideostreamingsoln-streamer:latest"
MODE="auto"

for arg in "$@"; do
    case "$arg" in
        --docker) MODE="docker" ;;
        --local) MODE="local" ;;
    esac
done

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

BUILD_DIR="./build"
BINARY="$BUILD_DIR/live_streamer"

has_native_ort() {
    local bin="$1"
    [ -x "$bin" ] && ((otool -L "$bin" 2>/dev/null || ldd "$bin" 2>/dev/null) | grep -qi onnxruntime)
}

if [ "$MODE" = "auto" ]; then
    if has_native_ort "$BINARY" && [ -f "./models/yolov8n.onnx" ]; then
        MODE="local"
    elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        MODE="docker"
    else
        MODE="local"
    fi
fi

echo "=========================================================="
echo " Starting Live Video Streaming Benchmark Suite ($MODE)"
echo "=========================================================="

if [ "$MODE" = "local" ]; then
    if [[ ! -x "$BINARY" ]]; then
        echo "[*] Binary $BINARY not found. Building via meson..."
        if [[ ! -d "$BUILD_DIR" ]]; then
            meson setup "$BUILD_DIR" --buildtype=debugoptimized
        fi
        meson compile -C "$BUILD_DIR"
    fi
    if ! has_native_ort "$BINARY"; then
        echo "ERROR: Native binary lacks ONNX Runtime; rerun with --docker for real YOLOv8n benchmarks."
        exit 1
    fi
    MODEL_PATH="$PROJECT_ROOT/models/yolov8n.onnx"
else
    if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
        echo "[*] Building Docker image $DOCKER_IMAGE..."
        docker compose build streamer
    fi
    MODEL_PATH="/opt/models/yolov8n.onnx"
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
    local log_file="$scenario_dir/run.log"

    if [ "$MODE" = "docker" ]; then
        local docker_env=()
        for kv in "${env_args[@]}"; do
            docker_env+=("-e" "$kv")
        done
        set +e
        docker run --rm \
            --entrypoint /app/live_streamer \
            -v "$BENCH_WORKSPACE:/bench" \
            "${docker_env[@]}" \
            -e HLS_OUTPUT_DIR="/bench/$mode_name/hls" \
            -e MAX_DURATION_SEC="$MAX_DURATION_SEC" \
            -e HLS_SEGMENT_DURATION="$HLS_SEGMENT_DURATION" \
            -e PROFILING_LOG_INTERVAL_SEC="$PROFILING_LOG_INTERVAL_SEC" \
            "$DOCKER_IMAGE" > "$log_file" 2>&1
        set -e
    else
        local time_cmd=""
        if [[ "$(uname)" == "Darwin" ]]; then
            time_cmd="/usr/bin/time -l"
        else
            time_cmd="/usr/bin/time -v"
        fi
        set +e
        env "${env_args[@]}" \
            HLS_OUTPUT_DIR="$scenario_dir/hls" \
            MAX_DURATION_SEC="$MAX_DURATION_SEC" \
            HLS_SEGMENT_DURATION="$HLS_SEGMENT_DURATION" \
            PROFILING_LOG_INTERVAL_SEC="$PROFILING_LOG_INTERVAL_SEC" \
            $time_cmd "$BINARY" > "$log_file" 2>&1
        set -e
    fi
}

# 1. Baseline (Sync ONNX Inference + Unpooled Allocations + Default Threads)
run_scenario "Baseline" \
    SOURCE_TYPE=lavfi \
    SOURCE_URL="testsrc2=size=1280x720:rate=30" \
    SOURCE_WIDTH=1280 \
    SOURCE_HEIGHT=720 \
    SOURCE_FPS=30 \
    TRANSCODE_MODE=always \
    AI_ENABLED=true \
    AI_MODEL_PATH="$MODEL_PATH" \
    AI_CONF_THRESHOLD=0.35 \
    SYNTHETIC_DETECTIONS=false \
    AI_MAX_FPS=0 \
    AI_SYNC_MODE=true \
    USE_BUFFER_POOL=false \
    ENCODER_THREADS=0 \
    AI_THREADS=2

# 2. Optimized (Async Ring-Buffer ONNX Inference + ByteBufferPool + Thread Caps)
run_scenario "Optimized" \
    SOURCE_TYPE=lavfi \
    SOURCE_URL="testsrc2=size=1280x720:rate=30" \
    SOURCE_WIDTH=1280 \
    SOURCE_HEIGHT=720 \
    SOURCE_FPS=30 \
    TRANSCODE_MODE=always \
    AI_ENABLED=true \
    AI_MODEL_PATH="$MODEL_PATH" \
    AI_CONF_THRESHOLD=0.35 \
    SYNTHETIC_DETECTIONS=false \
    AI_MAX_FPS=0 \
    AI_SYNC_MODE=false \
    USE_BUFFER_POOL=true \
    ENCODER_THREADS=2 \
    AI_THREADS=2

# 3. Smart Remux (Zero-Copy H.264 Pass-Through)
MP4_HOST="$BENCH_WORKSPACE/sample_input.mp4"
MP4_RUNTIME="$MP4_HOST"
if [ "$MODE" = "docker" ]; then
    MP4_RUNTIME="/bench/sample_input.mp4"
fi
echo "[*] Generating 15s H.264 MP4 source for Smart Remux..."
ffmpeg -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -t 15 -c:v libx264 -preset ultrafast -g 30 "$MP4_HOST" >/dev/null 2>&1

run_scenario "Smart_Remux" \
    SOURCE_TYPE=file \
    SOURCE_URL="$MP4_RUNTIME" \
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
bench_mode = "$MODE"
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
    ai_backend = "disabled"

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
        ai_backend = sdata.get("ai_backend", "disabled")
        if sdata.get("peak_rss_mb", 0.0) > 0:
            peak_rss = round(float(sdata["peak_rss_mb"]), 2)
        effective_fps = sdata.get("output_fps", sdata.get("ingest_fps", 0.0))
        uptime = sdata.get("uptime_sec", 0.0)
        if uptime > 0.5 and frames_processed > 0:
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
        effective_fps = round(frames_processed / float($MAX_DURATION_SEC), 2)

    results.append({
        "mode": sc.replace("_", " "),
        "benchmark_env": bench_mode,
        "ai_backend": ai_backend,
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
print("| Mode | Backend | Frames | Eff. FPS | Decode avg/p95 (ms) | AI Inf avg/p95 (ms) | Encode/Mux avg/p95 (ms) | Total Frame avg/p95 (ms) | Peak RSS (MB) |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
for r in results:
    print(f"| {r['mode']} | {r['ai_backend']} | {r['frames_processed']} | {r['effective_fps']:.2f} | "
          f"{r['decode_avg']:.3f} / {r['decode_p95']:.3f} | "
          f"{r['inference_avg']:.3f} / {r['inference_p95']:.3f} | "
          f"{r['encode_avg']:.3f} / {r['encode_p95']:.3f} | "
          f"{r['total_avg']:.3f} / {r['total_p95']:.3f} | {r['peak_rss_mb']:.2f} |")

max_val = max([r["total_p95"] for r in results] + [1.0])
scale_max = max(max_val * 1.2, 5.0)
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
  <text x="390" y="32" text-anchor="middle" class="title">Critical-Path Frame Latency: Avg (solid) vs P95 (shaded)</text>
  <text x="390" y="52" text-anchor="middle" class="subtitle">1280x720 @ 30fps, ONNX Runtime YOLOv8n CPU (lower is faster)</text>
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

max_total_avg = max(baseline_r["total_avg"], opt_r["total_avg"], remux_r["total_avg"], 33.33)
total_budget = max(40.0, max_total_avg * 1.18)

def stage_bar(y, title, r, scale_max_ms):
    bar_x0 = 165.0
    bar_w = 435.0
    scale = bar_w / scale_max_ms
    segs = [
        ("Decode", r["decode_avg"], "#38bdf8"),
        ("AI Sync" if r["mode"] == "Baseline" else "AI Async", r["inference_avg"] if r["mode"] == "Baseline" else 0.0, "#f59e0b"),
        ("Encode/Mux", r["encode_avg"], "#10b981"),
    ]
    out = [f'<text x="20" y="{y + 18}" class="label">{title}</text>']
    x = bar_x0
    for name, ms, col in segs:
        w = max(0.0, ms * scale)
        if w >= 1.5:
            draw_w = min(w, (bar_x0 + bar_w) - x)
            out.append(f'<rect x="{x:.1f}" y="{y}" width="{draw_w:.1f}" height="26" fill="{col}" rx="3"/>')
            if draw_w > 54:
                out.append(f'<text x="{x + draw_w/2:.1f}" y="{y + 17}" text-anchor="middle" class="seg">{name} {ms:.1f}ms</text>')
            x += w
    out.append(f'<text x="760" y="{y + 18}" text-anchor="end" class="val">{r["total_avg"]:.2f}ms (p95 {r["total_p95"]:.2f}ms)</text>')
    return "\n  ".join(out)

budget_x = 165.0 + (33.33 / total_budget) * 435.0

breakdown_svg = f'''<svg width="780" height="265" xmlns="http://www.w3.org/2000/svg">
  <style>
    .title {{ font: bold 16px sans-serif; fill: #f8fafc; }}
    .subtitle {{ font: 12px sans-serif; fill: #94a3b8; }}
    .label {{ font: bold 12px sans-serif; fill: #e2e8f0; }}
    .seg {{ font: bold 10px sans-serif; fill: #0f172a; }}
    .val {{ font: 11px monospace; fill: #cbd5e1; }}
    .budget {{ stroke: #f87171; stroke-width: 1.5; stroke-dasharray: 4; }}
  </style>
  <rect width="100%" height="100%" fill="#0f172a" rx="8"/>
  <text x="390" y="30" text-anchor="middle" class="title">Stage Latency Breakdown vs 33.3ms (30 FPS) Frame Budget</text>
  <text x="390" y="50" text-anchor="middle" class="subtitle">Critical-path wall time per frame with ONNX Runtime YOLOv8n</text>
  <line x1="{budget_x:.1f}" y1="68" x2="{budget_x:.1f}" y2="215" class="budget"/>
  <text x="{budget_x + 4:.1f}" y="72" class="subtitle">33.3ms budget</text>
  {stage_bar(82, "Baseline (Sync)", baseline_r, total_budget)}
  {stage_bar(132, "Optimized (Async)", opt_r, total_budget)}
  {stage_bar(182, "Smart Remux", remux_r, total_budget)}
  <text x="20" y="242" class="subtitle">Note: In Optimized mode, ONNX Runtime inference runs asynchronously off the critical path via DropOldestRingBuffer.</text>
</svg>'''

with open("docs/profiling/stage_latency_breakdown.svg", "w") as f:
    f.write(breakdown_svg)

print("[*] Wrote docs/profiling/stage_latency_breakdown.svg")
EOF

echo "=========================================================="
echo " Benchmark Complete!"
echo "=========================================================="
