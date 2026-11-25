**Skip the management interface—just serve the Web Player on `http://localhost:8999`.** The assignment only requires a web player supporting live streaming and playback, and building a management UI (REST control endpoints, dynamic state mutation, auth) in a two-day window adds unnecessary surface area without improving your evaluation score. Instead, keep all configuration declarative in `.env` and add a **read-only status bar** inside the player (connection state, live latency, FPS, inference time) fed by a lightweight `stats.json` file atomically updated by the C++ engine every second.

---

## 1. Amended Architecture (Zero MediaMTX, Pure Libav HLS-fMP4)

Removing MediaMTX makes the design leaner, faster to containerize, and stronger from a systems-engineering standpoint: your C++ binary owns the entire media pipeline end-to-end.

```
[IP Cam (RTSP) / Webcam (V4L2) / Mock RTSP Container]
                          │
                          ▼
┌──────────────────────────────────────────────────────────┐
│        C++ Streamer & AI Engine (Meson + Docker)         │
│                                                          │
│  [Libav Ingest] (AVIOInterruptCB + Backoff Reconnect)    │
│        │                                                 │
│        ├──(H.264 & AI=OFF)──────────────►[Zero-Copy Remux]
│        │                                        │        │
│        ▼ (Webcam / Non-H.264 / AI=ON)           │        │
│  [Decode] ──► (Bounded Ring Buffer)             │        │
│                     │                           │        │
│          ┌──────────┴──────────┐                │        │
│          ▼                     ▼                │        │
│   [Async AI Worker]     [Overlay + libx264]     │        │
│   ONNX Runtime C++       (zerolatency)          │        │
│   (YOLOv8n 320px) ──►          │                │        │
│                                ▼                ▼        │
│                   [Libav HLS-fMP4 Muxer]                 │
│           init.mp4 + seg_XXXXX.m4s + live.m3u8           │
│         + Sidecar Manager (playback.m3u8 & Pruner)       │
│         + Atomic Telemetry Writer (stats.json)           │
└──────────────────────────┬───────────────────────────────┘
                           │ Shared Docker Volume (/data/hls)
                           ▼
┌──────────────────────────────────────────────────────────┐
│               Nginx Static Server (:8999)                │
│  • /           -> Web Player (HTML + HLS.js)             │
│  • /hls/*.m3u8 -> no-cache headers (Live & Playback)     │
│  • /hls/*.m4s  -> immutable cache + Byte-Range support   │
│  • /hls/stats.json -> Live pipeline & AI metrics         │
└──────────────────────────────────────────────────────────┘

```

### Key Architectural Amendments

* **HLS.js over Video.js:** Choose **HLS.js**. It attaches directly to a standard HTML5 `<video>` element with ~40 lines of JS, natively handles **fMP4** segments and `#EXT-X-DISCONTINUITY` tags, exposes exact live-edge latency (`hls.latency`), and gives explicit hooks for `Hls.Events.ERROR` (`NETWORK_ERROR` / `MEDIA_ERROR`) auto-recovery—making outage resilience visibly demonstrable in the browser.


* **Conditional Transcoding (Smart Pass-Through):**
* **Remux Mode:** When `AI_ENABLED=false` and the incoming IP camera codec is already H.264/HEVC, bypass decode/encode completely and remux packets directly into HLS-fMP4 (`av_interleaved_write_frame`).
* **Transcode Mode:** Automatically engaged when `AI_ENABLED=true` (to bake bounding boxes into both live and recorded footage) or when the source is a V4L2 webcam (YUYV/MJPEG) or non-HLS codec.


* **Live + Playback Without Duplicate Storage:** Configure Libav's `hls` muxer with `hls_segment_type=fmp4`, `hls_time=2`, `hls_list_size=5`, and `hls_flags=independent_segments+discont_start` **without** `delete_segments` so `live.m3u8` stays a tight low-latency rolling window. A lightweight C++ segment-tracker appends each completed `.m4s` file into `playback.m3u8` (VOD/Event manifest) and deletes segments older than `RETENTION_SECONDS`, allowing the player to toggle between **Live** and **Playback** using the exact same disk segments.


* **Two-Sided Network Outage Handling:**
* *Upstream (Camera drop / hung socket):* `AVIOInterruptCB` enforces a hard read timeout; the capture loop reconnects using exponential backoff with jitter and injects `#EXT-X-DISCONTINUITY` on recovery so HLS.js resumes smoothly without a manual page refresh.


* *Downstream (Client offline / Nginx restart):* The C++ engine keeps writing fMP4 segments to local storage uninterrupted (zero lost footage), while HLS.js catches `NETWORK_ERROR` and retries automatically until the server/network returns.





---

## 2. `.env` Configuration Contract

A single `.env` file drives both `docker compose` (ports, device passthrough) and the C++ binary (`std::getenv` loaded into a validated `Config` struct at startup):

```env
# --- Server & Docker ---
WEB_PORT=8999
HLS_OUTPUT_DIR=/data/hls

# --- Input Source ---
# Modes: rtsp | v4l2
SOURCE_TYPE=rtsp
SOURCE_URL=rtsp://mock-camera:8554/live   # or /dev/video0 for v4l2
SOURCE_WIDTH=1280
SOURCE_HEIGHT=720
SOURCE_FPS=30

# --- Transcoding & HLS-fMP4 ---
TRANSCODE_MODE=auto                       # auto | always | remux
ENCODER_PRESET=ultrafast
ENCODER_BITRATE_KBPS=2000
ENCODER_THREADS=2
HLS_SEGMENT_DURATION=2
HLS_LIVE_LIST_SIZE=5
PLAYBACK_RETENTION_SEC=1800               # 30 min rolling DVR

# --- Local AI Inference ---
AI_ENABLED=true
AI_MODEL_PATH=/app/models/yolov8n_320.onnx
AI_CONF_THRESHOLD=0.45
AI_THREADS=2
AI_TARGET_CLASSES=person,car,truck,bicycle

# --- Resilience & Profiling ---
READ_TIMEOUT_MS=5000
RECONNECT_BASE_MS=1000
RECONNECT_MAX_MS=30000
PROFILING_LOG_INTERVAL_SEC=5

```

---

## 3. Meson Build & Repository Layout

Meson fits this 2-day sprint well—`pkg-config` resolution for Libav is effortless, and switching between release, profiling (`debugoptimized`), and sanitizers (`-Db_sanitize=address` / `thread`) is a one-line flag.

```text
live-stream-hls-cpp/
├── meson.build                     # Libav, ONNX Runtime, threads, GTest/Catch2
├── meson_options.txt               # Options: enable_ai, enable_tests
├── .env.example                    # Documented template (copied to .env)
├── docker-compose.yml              # streamer + nginx (:8999) + optional mock-cam
├── Dockerfile                      # Multi-stage: Meson build -> slim runtime + perf
├── config/
│   └── nginx.conf                  # Serves UI + /hls/ with CORS & cache rules
├── src/
│   ├── main.cpp                    # Signal handling (SIGINT/SIGTERM), lifecycle
│   ├── config.hpp                  # .env parser + validation
│   ├── capture/                    # Libav RAII wrappers, RTSP/V4L2, interrupt CB
│   ├── pipeline/                   # Bounded ring buffer, buffer pool, stage profiler
│   ├── inference/                  # ONNX Runtime async worker, letterbox, NMS, draw
│   └── muxer/                      # Conditional libx264 + HLS-fMP4 + playback.m3u8
├── web/
│   ├── index.html                  # Clean Live/Playback UI + status bar
│   └── player.js                   # HLS.js integration + network error recovery
├── tests/
│   ├── unit_tests.cpp              # Ring buffer, NMS, config, backoff tests
│   └── chaos_outage.sh             # Simulates camera drop + network cut[cite: 2]
├── docs/
│   ├── DESIGN.md                   # Architecture, trade-offs, horizontal scaling[cite: 2]
│   └── profiling/                  # Before/After flamegraphs & latency tables[cite: 2]
└── README.md                       # 1-command setup, webcam/IP cam guide, results[cite: 2]

```

---

## 4. Aggressive 2-Day Execution Plan

To finish cleanly in **48 hours** without cutting profiling or documentation, lock down the working video pipeline by Day 1 afternoon and reserve the second half of Day 2 strictly for profiling, failure testing, and docs.

| Block | Scope | Concrete Exit Criteria |
| --- | --- | --- |
| **Day 1 — Morning (4h)** | **Skeleton, Meson, Docker & Capture** | Create `meson.build`, `Dockerfile`, `docker-compose.yml` (with a lightweight FFmpeg RTSP mock camera container), `.env` parser, and RAII Libav ingest supporting RTSP + V4L2. *Check:* C++ binary reads frames from mock RTSP and webcam cleanly.

 |
| **Day 1 — Afternoon (4h)** | **HLS-fMP4 Muxer + Nginx + HLS.js Player** | Implement remux/transcode paths (`libx264` zerolatency), Libav HLS-fMP4 muxer (`live.m3u8` + `playback.m3u8` + retention pruner), Nginx on `:8999`, and `web/player.js` with Live & Playback tabs. *Check:* `docker compose up` plays live video and recorded playback at `http://localhost:8999`.

 |
| **Day 1 — Evening (3h)** | **Async AI Inference (ONNX Runtime)** | Export YOLOv8n (`320×320` FP32/INT8) to `.onnx`. Build bounded ring buffer + async inference thread + lightweight YUV/RGB box drawer (`person`, `car`, etc.) + `stats.json` writer. *Check:* Smooth live stream with real-time bounding boxes and no frame-rate lockup.

 |
| **Day 2 — Morning (4h)** | **Outage Handling & Automated Testing** | Add `AVIOInterruptCB` timeout, exponential backoff reconnect, `#EXT-X-DISCONTINUITY` continuity, and HLS.js network-error auto-recovery. Write Meson unit tests + `tests/chaos_outage.sh` (kill/restart mock camera, block network). *Check:* Pipeline survives repeated disconnects with zero crashes.

 |
| **Day 2 — Afternoon (4h)** | **Profiling & Optimization Pass** | Build with `-Dbuildtype=debugoptimized`. Capture **Baseline** metrics (synchronous AI / default threads / per-frame allocs) via `perf` flamegraph + stage latency histograms. Apply **Optimizations** (async worker, pre-allocated frame/tensor pools, explicit thread caps for `libx264` vs ONNX) and capture **Optimized** flamegraph + ASan/TSan clean run. *Check:* Reproducible Before/After table and SVGs in `docs/profiling/`.

 |
| **Day 2 — Evening (3h)** | **Documentation & Cold-Start Dry Run** | Write `README.md` (setup, `.env` flags, webcam vs. IP camera run steps, benchmark results) and `docs/DESIGN.md` (Libav HLS-fMP4 rationale, failure modes, horizontal scaling via sharded ingest pods + CDN/S3). Clone fresh into a clean folder and verify `cp .env.example .env && docker compose up` works on the first try.

 |

---

