# Live Video Streamer

C++20 live video ingest, HLS-fMP4 segmenter, and local ONNX Runtime object detection pipeline built directly on FFmpeg Libav (`libavformat`, `libavcodec`, `libavdevice`, `libswscale`). Supports concurrent multi-stream ingest (`STREAMS`), pass-through H.264/HEVC remuxing, conditional `libx264` transcoding with direct YUV420P bounding-box overlays, rolling DVR playback, and client-side object search.

---

## Contents
1. [Features](#1-features)
2. [Architecture](#2-architecture)
3. [Quick Start (Docker)](#3-quick-start-docker)
4. [Web Player](#4-web-player)
5. [Native Build (Meson)](#5-native-build-meson)
6. [Configuration](#6-configuration)
7. [Tests](#7-tests)
8. [Benchmarks](#8-benchmarks)

---

## 1. Features

- **Direct Libav & Multi-Stream Pipeline**: Opens one or many RTSP, V4L2, AVFoundation, `lavfi`, or file inputs concurrently (one isolated pipeline thread per stream via `STREAMS`) using `avformat_open_input` / `avcodec_send_packet` / `avcodec_receive_frame` / `av_interleaved_write_frame`. Each stream writes to `<HLS_OUTPUT_DIR>/<id>/`, with a root `streams.json` manifest for player discovery.
- **Smart Remux vs. Transcode**:
  - `TRANSCODE_MODE=auto` remuxes H.264/HEVC streams without decoding when AI is disabled.
  - Falls back to `libx264` (`preset=ultrafast`) when transcoding is required or when AI overlays are drawn onto decoded frames.
- **Shared-Segment Live + DVR HLS (`fMP4`)**:
  - `live.m3u8`: rolling live playlist (`HLS_LIVE_LIST_SIZE=5`, `HLS_SEGMENT_DURATION=2s`).
  - `playback.m3u8`: DVR playlist retaining up to `PLAYBACK_RETENTION_SEC=1800` of the same `.m4s` segments while background pruning protects active live segments.
- **Outage Recovery**:
  - Upstream: `AVIOInterruptCB` read deadlines with jittered exponential backoff (`RECONNECT_BASE_MS`, `RECONNECT_MAX_MS`) and `#EXT-X-DISCONTINUITY` + versioned `init_<epoch>.mp4` rotation on reconnect.
  - Downstream: HLS.js network/media retry handlers and stall recovery in the browser player.
- **Local ONNX Inference & YUV Overlay**:
  - Asynchronous inference worker fed by a capacity-2 drop-oldest ring buffer (`DropOldestRingBuffer`) and recycled frame buffers (`ByteBufferPool`).
  - YOLOv8 post-processing + per-class NMS with bounding boxes and labels drawn directly onto YUV420P planes (no secondary RGB roundtrip).
  - Writes throttled detections to `events.json` and pipeline metrics to `stats.json` via atomic temp-file rename.

---

## 2. Architecture

```text
+---------------------------------------------------------------------------------+
|                                  INPUT SOURCES                                  |
|  [ RTSP cam1..N ]  [ V4L2 Webcam ]  [ AVFoundation ]  [ Lavfi ]  [ MP4 Files ]  |
+----------------------------------------+----------------------------------------+
                                         | (one isolated thread per stream)
                                         v
+---------------------------------------------------------------------------------+
|                         C++20 CORE ENGINE (live_streamer)                       |
|                                                                                 |
|  +-----------------------+     +-----------------+     +---------------------+  |
|  | SourceCapture         | --> | Libav Demuxer   | --> | Remux / Transcode?  |  |
|  | (AVIOInterruptCB +    |     | (AVPacket)      |     +----+-----------+----+  |
|  |  ExponentialBackoff)  |     +-----------------+          |           |       |
|  +-----------------------+                        Transcode |           | Remux |
|                                                   / AI      |           |       |
|  +----------------------------------------------------+     v           |       |
|  |       AI WORKER THREAD (shared Ort::Env)           |  +-----------+  |       |
|  |                                                    |  | Libav     |  |       |
|  |  DropOldestRingBuffer (cap=2) <== RGB24 frame copy |<-+ Decoder + |  |       |
|  |         |                        (ByteBufferPool)  |  | sws_scale |  |       |
|  |         v                                          |  +-----+-----+  |       |
|  |  ONNX Runtime Ort::Session (YOLOv8) + NMS          |        |YUV420P |       |
|  |         |                                          |        v        |       |
|  |         +---> latest_detections                    |  +-----------+  |       |
|  |                     |                              |  | YUV420P   |  |       |
|  +---------------------+------------------------------+  | Overlay   |  |       |
|                        +-------------------------------->| (boxes)   |  |       |
|                        |                                 +-----+-----+  |       |
|                        v (events)                              |        |       |
|  +----------------------------------------------------+  +-----+-----+  |       |
|  | PlaybackManager & StatsWriter                      |  | libx264   |  |       |
|  | (reads live.m3u8; writes playback.m3u8,            |  | Encoder   |  |       |
|  |  events.json, stats.json atomically)               |  +-----+-----+  |       |
|  +-------------------------+--------------------------+        |        |       |
|                            ^                                   v        v       |
|                            |  sync                       +-------------------+  |
|                            +-----------------------------| HlsMuxer (fMP4)   |  |
|                                                          | live.m3u8 + .m4s  |  |
|                                                          +---------+---------+  |
+--------------------------------------------------------------------+------------+
                                                                     |
                                                                     v
+---------------------------------------------------------------------------------+
|                      SHARED VOLUME (/data/hls -> hls_data)                      |
|  streams.json                                                                   |
|  <id>/ : live.m3u8 | playback.m3u8 | init_*.mp4 | seg_*.m4s | *.json            |
+----------------------------------------+----------------------------------------+
                                         |  HTTP / HLS-fMP4 (Nginx root /data)
                                         v
+---------------------------------------------------------------------------------+
|                           NGINX HTTP SERVER (:8999)                             |
+----------------------------------------+----------------------------------------+
                                         |
                                         v
+---------------------------------------------------------------------------------+
|                               BROWSER WEB CLIENT                                |
|  * Stream Selector (reads /hls/streams.json, 1-9 hotkeys, legacy fallback)      |
|  * HLS.js / Native HLS Player (Live live.m3u8 & DVR playback.m3u8)              |
|  * Status Telemetry Bar (polls stats.json)                                      |
|  * Object Search & Click-to-Seek DVR (polls events.json)                        |
+---------------------------------------------------------------------------------+
```

---

## 3. Quick Start (Docker)

### Docker Compose

Run the full stack (`mock-camera` simulator, C++ `streamer`, and `nginx` web UI):

```bash
docker compose up --build
```

Then open `http://localhost:8999`.

Defaults from a fresh clone:
- `docker-compose.yml` loads `.env.example` and applies `.env` overrides if present (`STREAMS` defaults to `cam1` + `cam2`).
- `Dockerfile` bundles COCO-80 `yolov8n.onnx` at `/opt/models/yolov8n.onnx` (and `AiWorker` falls back to it if `AI_MODEL_PATH` is missing).
- `mock-camera` (`bluenviron/mediamtx`) serves two RTSP paths: `cam1` picks the 1st `.mp4` in `media/` (else synthetic `testsrc2`) and `cam2` picks the 2nd `.mp4` in `media/` (else synthetic `testsrc`), using `-c:v copy` for H.264 or `libx264` otherwise.

To test with your own videos or model:
- Copy one or more `.mp4` files into `media/` (1st feeds `cam1`, 2nd feeds `cam2`), or point `STREAMS` in `.env` at your RTSP/file sources (`id=type|url,...`).
- Copy an `.onnx` model into `models/` and set `AI_MODEL_PATH=/app/models/<model>.onnx` in `.env`.

### Standalone Container

`live_streamer` writes HLS playlists and `.m4s` segments directly to `HLS_OUTPUT_DIR` (`/data/hls`) and does not embed an HTTP server. To run just the engine against a generated source:

```bash
docker build -t live-streamer .
docker run --rm -v "$(pwd)/hls_out:/data/hls" \
  -e SOURCE_TYPE=lavfi \
  -e SOURCE_URL="testsrc2=size=1280x720:rate=30" \
  live-streamer
```

---

## 4. Web Player

Served from [web/](web/) via Nginx on port `8999`:
- **Stream Selector**: Discovers active feeds from `/hls/streams.json` (with fallback to single-stream `/hls/live.m3u8`), switches playlists/stats/events cleanly via header dropdown or number keys `1`–`9`, and resets DVR/search state per stream.
- **Live / Playback (DVR) Toggle**: Switches between `live.m3u8` and `playback.m3u8` without snapping DVR seeks back to the live edge.
- **Object Search**: Filters detection entries from `events.json` by class tags (`AND`/`OR`) and minimum confidence; clicking a result seeks directly into `playback.m3u8`.
- **Telemetry Bar**: Displays stream state, mode, live latency / DVR position, ingest & output FPS, AI inference timing, active labels, and reconnect/DVR counters from `stats.json`.
- **Keyboard Shortcuts**: `1`–`9` (switch stream), `Space` / `K` (play/pause), `M` (mute), `F` (fullscreen), `L` (jump to live edge), `←` / `→` (seek ±5s in DVR mode).

---

## 5. Native Build (Meson)

### Dependencies

- **macOS (Homebrew)**:
  ```bash
  brew install meson ninja ffmpeg onnxruntime
  ```
- **Ubuntu / Debian**:
  ```bash
  sudo apt-get update && sudo apt-get install -y \
    meson ninja-build pkg-config libavformat-dev libavcodec-dev \
    libavutil-dev libavdevice-dev libswscale-dev libx264-dev
  ```

### Compile & Test

```bash
meson setup build --buildtype=debugoptimized
meson compile -C build
meson test -C build --print-errorlogs
```

`.clangd` is configured to read `build/compile_commands.json` generated by `meson setup build`.

### Run Locally

```bash
mkdir -p hls_out
SOURCE_TYPE=lavfi SOURCE_URL="testsrc2=size=1280x720:rate=30" HLS_OUTPUT_DIR=./hls_out ./build/live_streamer
```

Point Nginx or any static HTTP server at `web/` and alias `/hls/` to `hls_out/`.

---

## 6. Configuration

Variables loaded from `.env` (or process environment, which overrides `.env`):

| Variable | Default | Description |
| :--- | :--- | :--- |
| `WEB_PORT` | `8999` | Host port mapped to Nginx. |
| `HLS_OUTPUT_DIR` | `/data/hls` | Output directory for `streams.json` and `.m3u8`/`.mp4`/`.m4s`/`.json` files (`<HLS_OUTPUT_DIR>/<id>/` when `STREAMS` is set). |
| `STREAMS` | *(empty in binary; `cam1`+`cam2` in `.env.example`)* | Comma-separated multi-stream list (`id=type\|url`, `id=url`, or bare URL). When empty, runs a single pipeline using `SOURCE_TYPE`/`SOURCE_URL`. |
| `SOURCE_TYPE` | `rtsp` | Capture backend (`rtsp`, `v4l2`, `avfoundation`, `lavfi`, `file`) for single-stream mode or default per-stream type. |
| `SOURCE_URL` | `rtsp://mock-camera:8554/cam1` | Single-stream RTSP URI, device index/path, file path, or `lavfi` graph. |
| `SOURCE_WIDTH` | `1280` | Capture/output width in pixels (fallback if container has no metadata). |
| `SOURCE_HEIGHT` | `720` | Capture/output height in pixels. |
| `SOURCE_FPS` | `30` | Target framerate for device/synthetic capture. |
| `TRANSCODE_MODE` | `auto` | `auto` (remux when AI off and H.264/HEVC), `always`, or `remux`. |
| `ENCODER_PRESET` | `ultrafast` | `libx264` preset. |
| `ENCODER_BITRATE_KBPS` | `2000` | Target `libx264` bitrate in kbps. |
| `ENCODER_THREADS` | `1` (`.env.example`) / `2` (binary default) | `libx264` encoder thread count. |
| `HLS_SEGMENT_DURATION` | `2` | Target HLS segment duration in seconds. |
| `HLS_LIVE_LIST_SIZE` | `5` | Number of segments kept in `live.m3u8`. |
| `PLAYBACK_RETENTION_SEC` | `1800` | Rolling DVR retention window in seconds. |
| `AI_ENABLED` | `true` | Enable ONNX Runtime object detection and YUV overlay. |
| `AI_MODEL_PATH` | `/opt/models/yolov8n.onnx` | Path to YOLOv8 `.onnx` model. |
| `AI_CONF_THRESHOLD` | `0.35` | Minimum class confidence score. |
| `AI_THREADS` | `1` (`.env.example`) / `2` (binary default) | ONNX Runtime intra-op thread count (`allow_spinning=0`). |
| `AI_MAX_FPS` | `3` | Maximum async AI inference submissions per stream per second (`0` = unlimited). |
| `AI_TARGET_CLASSES` | `person,car,truck,bicycle,motorcycle,bus` | Comma-separated COCO labels to detect (`*` for all). |
| `AI_SYNC_MODE` | `false` | Run inference inline on pipeline thread (`true`) or async (`false`). |
| `USE_BUFFER_POOL` | `true` | Recycle RGB inference buffers via `ByteBufferPool`. |
| `SYNTHETIC_DETECTIONS` | `false` | Generate synthetic test boxes when no `.onnx` model is loaded. |
| `MAX_DURATION_SEC` | `0` | Exit cleanly after N seconds (`0` runs until `SIGINT`/`SIGTERM`). |
| `READ_TIMEOUT_MS` | `5000` | `AVIOInterruptCB` packet read timeout in ms. |
| `RECONNECT_BASE_MS` | `1000` | Initial reconnect backoff delay in ms. |
| `RECONNECT_MAX_MS` | `30000` | Maximum reconnect backoff delay in ms. |
| `PROFILING_LOG_INTERVAL_SEC` | `5` | Interval in seconds for stdout telemetry summary logs. |

---

## 7. Tests

- **Unit & Module Tests ([tests/unit_tests.cpp](tests/unit_tests.cpp))**:
  Covers `.env` parsing/validation, `DropOldestRingBuffer`, `ByteBufferPool`, `ExponentialBackoff`, YOLOv8 letterbox/decode/NMS, YUV420P overlay clipping, `PlaybackManager` (DVR stitching, discontinuities, segment pruning, atomic JSON), and `StageProfiler` percentiles. Runs during every `docker build` and via `meson test -C build --print-errorlogs`.
- **AddressSanitizer**:
  ```bash
  meson setup build_asan --buildtype=debugoptimized -Db_sanitize=address
  meson compile -C build_asan && meson test -C build_asan --print-errorlogs
  ```
- **Chaos Outage Recovery ([tests/chaos_outage.sh](tests/chaos_outage.sh))**:
  Drops the upstream feed mid-stream and checks exponential backoff reconnects and `#EXT-X-DISCONTINUITY` emission in `playback.m3u8`:
  ```bash
  ./tests/chaos_outage.sh --mode local
  ./tests/chaos_outage.sh --mode docker
  ```

---

## 8. Benchmarks

Measured inside Ubuntu 24.04 ARM64 Docker with ONNX Runtime 1.20.1 (`yolov8n.onnx`, `640x640` input) at **1280×720 @ 30fps** (raw results in [docs/profiling/latest_results.json](docs/profiling/latest_results.json)):

| Mode | AI Backend | Frames | AI Inferences | FPS | Decode avg/p95 (ms) | Inference avg/p95 (ms) | Encode avg/p95 (ms) | Total Frame avg/p95 (ms) | Peak RSS (MB) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Baseline** | `onnxruntime` | 215 | 215 | 17.86 | 0.001 / 0.001 | 52.684 / 56.669 | 1.345 / 1.675 | **55.567 / 59.897** | 144.28 |
| **Optimized** | `onnxruntime` | 365 | 213 | 30.35 | 0.001 / 0.003 | 55.571 / 57.494 | 1.359 / 1.525 | **2.036 / 2.251** | 149.56 |
| **Smart Remux** | `disabled` | 365 | 0 | 30.35 | 0.000 / 0.000 | 0.000 / 0.000 | 0.149 / 0.070 | **0.149 / 0.071** | **62.73** |

Further details:
- [Benchmark Report](docs/profiling/benchmark_report.md)
- [System Design](docs/DESIGN.md)
