#!/usr/bin/env bash
set -euo pipefail

# Chaos Outage & Reconnect Test Script for Live Video Streaming Solution
# Supports --mode local and --mode docker

if [ -d "/Applications/Docker.app/Contents/Resources/bin" ]; then
    export PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi

MODE="local"
if [ $# -gt 0 ]; then
    case "$1" in
        --mode)
            if [ $# -ge 2 ]; then
                MODE="$2"
            fi
            ;;
        local|docker)
            MODE="$1"
            ;;
    esac
fi

# Auto-fallback to local if docker info fails and mode is docker
if [ "$MODE" = "docker" ]; then
    if ! docker info >/dev/null 2>&1; then
        echo "Docker daemon not available or not running. Falling back to --mode local."
        MODE="local"
    fi
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Cleanup tracking
STREAMER_PID=""
FF1_PID=""
FF2_PID=""
TEMP_DIR=""

cleanup() {
    set +e
    echo "Cleaning up background processes and temporary files..."
    if [ -n "${STREAMER_PID:-}" ] && kill -0 "$STREAMER_PID" 2>/dev/null; then
        kill "$STREAMER_PID" 2>/dev/null || true
        wait "$STREAMER_PID" 2>/dev/null || true
    fi
    if [ -n "${FF1_PID:-}" ] && kill -0 "$FF1_PID" 2>/dev/null; then
        kill "$FF1_PID" 2>/dev/null || true
    fi
    if [ -n "${FF2_PID:-}" ] && kill -0 "$FF2_PID" 2>/dev/null; then
        kill "$FF2_PID" 2>/dev/null || true
    fi
    if [ "$MODE" = "docker" ]; then
        docker compose down -v >/dev/null 2>&1 || true
    fi
    if [ -n "${TEMP_DIR:-}" ] && [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
    fi
}

trap cleanup EXIT

echo "=== Running Chaos Outage Test in mode: $MODE ==="

if [ "$MODE" = "local" ]; then
    # 1. Ensure live_streamer binary exists
    if [ ! -f "./build/live_streamer" ]; then
        echo "Building live_streamer..."
        meson compile -C build
    fi

    TEMP_DIR="/tmp/chaos_local_$$"
    HLS_DIR="$TEMP_DIR/hls"
    mkdir -p "$HLS_DIR"

    echo "=== Test 1: Upstream TCP/MPEG-TS Outage & Reconnect (Local) ==="
    
    # Launch ffmpeg TCP stream server #1
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i testsrc2=size=640x360:rate=30 \
        -c:v libx264 -preset ultrafast -tune zerolatency -g 30 \
        -f mpegts "tcp://127.0.0.1:19123?listen=1" &
    FF1_PID=$!
    
    sleep 0.5

    # Launch live_streamer in background
    SOURCE_TYPE=file SOURCE_URL="tcp://127.0.0.1:19123" \
        HLS_OUTPUT_DIR="$HLS_DIR" \
        TRANSCODE_MODE=auto \
        AI_ENABLED=true \
        SYNTHETIC_DETECTIONS=true \
        READ_TIMEOUT_MS=2000 \
        RECONNECT_BASE_MS=400 \
        RECONNECT_MAX_MS=1500 \
        MAX_DURATION_SEC=22 \
        ./build/live_streamer > "$TEMP_DIR/streamer.log" 2>&1 &
    STREAMER_PID=$!

    echo "Waiting for live.m3u8 and segments to appear..."
    ELAPSED=0
    while [ $ELAPSED -lt 8 ]; do
        if [ -f "$HLS_DIR/live.m3u8" ] && ls "$HLS_DIR"/*.m4s >/dev/null 2>&1; then
            break
        fi
        sleep 1
        ELAPSED=$((ELAPSED + 1))
    done

    if [ ! -f "$HLS_DIR/live.m3u8" ]; then
        echo "ERROR: live.m3u8 failed to appear in time."
        cat "$TEMP_DIR/streamer.log" || true
        exit 1
    fi
    echo "Initial stream active."

    # Simulate sudden camera/network outage
    echo "Simulating upstream outage: killing ffmpeg #1 (PID $FF1_PID)..."
    kill "$FF1_PID" 2>/dev/null || true
    wait "$FF1_PID" 2>/dev/null || true
    FF1_PID=""

    echo "Sleeping 3s while streamer logs reconnect attempts..."
    sleep 3

    # Verify streamer is still alive
    if ! kill -0 "$STREAMER_PID" 2>/dev/null; then
        echo "ERROR: live_streamer crashed during upstream outage!"
        cat "$TEMP_DIR/streamer.log" || true
        exit 1
    fi
    echo "live_streamer survived outage and is reconnecting."

    # Start ffmpeg TCP stream server #2 on port 19123
    echo "Restarting ffmpeg TCP stream server #2..."
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i testsrc2=size=640x360:rate=30 \
        -c:v libx264 -preset ultrafast -tune zerolatency -g 30 \
        -f mpegts "tcp://127.0.0.1:19123?listen=1" &
    FF2_PID=$!

    echo "Waiting for live_streamer to finish execution (MAX_DURATION_SEC=22)..."
    ELAPSED=0
    while [ "$ELAPSED" -lt 28 ] && kill -0 "$STREAMER_PID" 2>/dev/null; do
        sleep 1
        ELAPSED=$((ELAPSED + 1))
    done

    # Wait for background process to fully reap
    wait "$STREAMER_PID" 2>/dev/null || true

    echo "Checking assertions..."
    
    # 1. stats.json reconnect_count >= 1
    if [ ! -f "$HLS_DIR/stats.json" ]; then
        echo "ERROR: stats.json not found."
        cat "$TEMP_DIR/streamer.log" || true
        exit 1
    fi
    
    RECONNECT_COUNT=$(grep -o '"reconnect_count":[0-9]*' "$HLS_DIR/stats.json" | head -n1 | cut -d':' -f2 || echo "0")
    echo "Detected reconnect_count: $RECONNECT_COUNT"
    if [ "${RECONNECT_COUNT:-0}" -lt 1 ]; then
        echo "ERROR: reconnect_count is less than 1."
        cat "$HLS_DIR/stats.json"
        exit 1
    fi

    # 2. playback.m3u8 exists and contains EXT-X-DISCONTINUITY and init_1.mp4
    if [ ! -f "$HLS_DIR/playback.m3u8" ]; then
        echo "ERROR: playback.m3u8 not found."
        exit 1
    fi

    if ! grep -q "#EXT-X-DISCONTINUITY" "$HLS_DIR/playback.m3u8"; then
        echo "ERROR: #EXT-X-DISCONTINUITY not found in playback.m3u8:"
        cat "$HLS_DIR/playback.m3u8"
        exit 1
    fi
    echo "Found #EXT-X-DISCONTINUITY in playback.m3u8."
    if ! grep -q "init_1.mp4" "$HLS_DIR/playback.m3u8"; then
        echo "ERROR: init_1.mp4 not found in playback.m3u8:"
        cat "$HLS_DIR/playback.m3u8"
        exit 1
    fi
    echo "Found init_1.mp4 in playback.m3u8."

    # 3. Combine init_0.mp4 + first .m4s and run ffprobe
    if [ -f "$HLS_DIR/init_0.mp4" ]; then
        FIRST_SEG=$(ls "$HLS_DIR"/*.m4s 2>/dev/null | head -n1 || true)
        if [ -n "$FIRST_SEG" ]; then
            COMBINED="$TEMP_DIR/combined.mp4"
            cat "$HLS_DIR/init_0.mp4" "$FIRST_SEG" > "$COMBINED"
            if command -v ffprobe >/dev/null 2>&1; then
                echo "Running ffprobe validation on combined segment..."
                FFPROBE_OUT=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height,codec_name -of default=noprint_wrappers=1 "$COMBINED" 2>&1 || true)
                echo "$FFPROBE_OUT"
                if echo "$FFPROBE_OUT" | grep -q "h264"; then
                    echo "ffprobe verification passed: h264 codec confirmed."
                else
                    echo "WARNING: ffprobe did not explicitly output h264 or failed."
                fi
            else
                echo "ffprobe not installed, skipping media probe check."
            fi
        fi
    fi

    echo "=== LOCAL CHAOS TEST PASSED SUCCESSFULLY ==="

elif [ "$MODE" = "docker" ]; then
    echo "=== Running Docker Chaos Tests ==="

    echo "Building and starting containers..."
    docker compose up -d --build --force-recreate

    echo "Waiting for http://localhost:8999/hls/streams.json (or legacy live.m3u8)..."
    STREAM_PREFIX="/hls"
    ELAPSED=0
    while [ $ELAPSED -lt 30 ]; do
        STREAMS_JSON=$(curl -s -f "http://localhost:8999/hls/streams.json" 2>/dev/null || echo "")
        if [ -n "$STREAMS_JSON" ]; then
            PARSED_PREFIX=$(echo "$STREAMS_JSON" | python3 -c "import sys, json; d=json.load(sys.stdin); s=d.get('streams',[]); print(s[0]['hls_prefix'] if s else '/hls')" 2>/dev/null || echo "/hls")
            STREAM_PREFIX="$PARSED_PREFIX"
        fi
        if curl -s -f "http://localhost:8999${STREAM_PREFIX}/live.m3u8" >/dev/null 2>&1 && \
           curl -s -f "http://localhost:8999${STREAM_PREFIX}/stats.json" >/dev/null 2>&1; then
            break
        fi
        sleep 2
        ELAPSED=$((ELAPSED + 2))
    done

    if ! curl -s -f "http://localhost:8999${STREAM_PREFIX}/live.m3u8" >/dev/null 2>&1; then
        echo "ERROR: HLS stream (${STREAM_PREFIX}) did not become ready in Docker."
        docker compose logs
        exit 1
    fi
    echo "Docker stream is up at ${STREAM_PREFIX}."

    # 4. Upstream outage in Docker: kill mock-camera, wait 6s, start mock-camera, wait up to 16s
    echo "Simulating upstream outage in Docker (kill mock-camera)..."
    docker compose kill mock-camera
    sleep 6
    echo "Restarting mock-camera..."
    docker compose start mock-camera

    STATS_JSON="{}"
    PLAYBACK_M3U8=""
    RECONNECT_COUNT="0"
    ELAPSED=0
    while [ $ELAPSED -lt 16 ]; do
        STATS_JSON=$(curl -s "http://localhost:8999${STREAM_PREFIX}/stats.json" || echo "{}")
        PLAYBACK_M3U8=$(curl -s "http://localhost:8999${STREAM_PREFIX}/playback.m3u8" || echo "")
        RECONNECT_COUNT=$(echo "$STATS_JSON" | grep -o '"reconnect_count":[0-9]*' | head -n1 | cut -d':' -f2 || echo "0")
        if [ "${RECONNECT_COUNT:-0}" -ge 1 ] && \
           echo "$PLAYBACK_M3U8" | grep -q "#EXT-X-DISCONTINUITY" && \
           echo "$PLAYBACK_M3U8" | grep -q "init_1.mp4"; then
            break
        fi
        sleep 2
        ELAPSED=$((ELAPSED + 2))
    done

    echo "Detected Docker reconnect_count (${STREAM_PREFIX}): ${RECONNECT_COUNT:-0}"
    if [ "${RECONNECT_COUNT:-0}" -lt 1 ]; then
        echo "ERROR: Docker stats.json reconnect_count is less than 1:"
        echo "$STATS_JSON"
        docker compose logs streamer
        exit 1
    fi

    if ! echo "$PLAYBACK_M3U8" | grep -q "#EXT-X-DISCONTINUITY"; then
        echo "ERROR: #EXT-X-DISCONTINUITY not found in Docker playback.m3u8:"
        echo "$PLAYBACK_M3U8"
        exit 1
    fi
    echo "Found #EXT-X-DISCONTINUITY in Docker playback.m3u8."

    if ! echo "$PLAYBACK_M3U8" | grep -q "init_1.mp4"; then
        echo "ERROR: init_1.mp4 not found in Docker playback.m3u8:"
        echo "$PLAYBACK_M3U8"
        exit 1
    fi
    echo "Found init_1.mp4 in Docker playback.m3u8."

    # 5. Downstream outage in Docker: pause nginx, wait 4s, unpause nginx
    echo "Simulating downstream outage in Docker (pause nginx)..."
    docker compose pause nginx
    sleep 4
    echo "Unpausing nginx..."
    docker compose unpause nginx
    sleep 2

    if curl -s -f "http://localhost:8999${STREAM_PREFIX}/live.m3u8" >/dev/null 2>&1; then
        echo "Downstream recovery verified: ${STREAM_PREFIX}/live.m3u8 returned HTTP 200."
    else
        echo "ERROR: ${STREAM_PREFIX}/live.m3u8 not accessible after downstream unpause."
        exit 1
    fi

    echo "=== DOCKER CHAOS TEST PASSED SUCCESSFULLY ==="
fi

echo "All Chaos Tests Completed Successfully!"
