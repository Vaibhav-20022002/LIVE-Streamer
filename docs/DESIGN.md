# Live Video Streamer — System Design

## 1. Overview

This document describes the architecture of `live_streamer`, a C++20 Libav (`libavformat` / `libavcodec` / `libavdevice` / `libswscale`) streaming and local AI inference engine packaged in a static-HTTP HLS-fMP4 layout.

- **Direct Libav & Multi-Stream Pipeline**: The core binary handles stream ingest, demuxing, decoding, encoding, and HLS-fMP4 muxing directly through Libav C APIs without an intermediary media server. When `STREAMS` is configured, `main()` spawns one isolated pipeline thread per stream while sharing a single ONNX Runtime `Ort::Env`. (`docker-compose.yml` uses MediaMTX only as an optional test camera simulator pushing RTSP.)
- **HLS-fMP4 Output**: Writes `init_<epoch>.mp4` + `.m4s` fragments (`HLS_SEGMENT_DURATION=2s` by default), paired with rolling `live.m3u8` and DVR `playback.m3u8` playlists (`<HLS_OUTPUT_DIR>/` for single stream, or `<HLS_OUTPUT_DIR>/<id>/` plus `<HLS_OUTPUT_DIR>/streams.json` in multi-stream mode).
- **Direct YUV420P AI Overlay**: Runs YOLOv8 ONNX Runtime inference on a background thread and draws boxes/labels directly onto decoded YUV420P frames without a second RGB conversion.
- **Two-Sided Outage Handling**: Combines `AVIOInterruptCB` deadlines and jittered exponential backoff in C++ with retry/stall watchdogs in `web/player.js`.

---

## 2. Streaming & Packaging Pipeline

### 2.1 Smart Pass-Through vs. Conditional Transcoding

```text
+-------------------------------------+
|   RTSP / Camera Stream Ingest       |
+------------------+------------------+
                   |
                   v
           [ AI Enabled? ]
            /           \
         (No)           (Yes)
          /               \
         v                 v
[ Codec H.264/HEVC? ]   [ Libav Decode + sws_scale ]
     /        \         [ (YUV420P + RGB24)        ]
  (Yes)       (No)                 |
   /            \                  v
  v              v      [ Async AI Inference +     ]
[ Smart Remux ]  |      [ YUV420P Direct Overlay   ]
[ (Zero-Copy) ]  |                 |
  |              v                 |
  |     [ Libav Decode + ]         |
  |     [ sws_scale(YUV) ]         |
  |              |                 |
  |              +--------+--------+
  |                       |
  |                       v
  |              [ libx264 Transcode ]
  |              [ (ultrafast)       ]
  |                       |
  +-----------+-----------+
              |
              v
     +------------------+
     | HLS-fMP4 Muxer   |
     +------------------+
```

1. **Smart Remux (`AI_ENABLED=false` and compatible H.264/HEVC input under `TRANSCODE_MODE=auto` or `remux`)**: Demuxed packets are rescaled and written directly to HLS-fMP4 without decoding or re-encoding.
2. **Transcode Path (`AI_ENABLED=true`, `TRANSCODE_MODE=always`, or non-H.264/HEVC source)**: Decoded frames are converted via `libswscale` to YUV420P (and RGB24 for AI when enabled), annotated directly in YUV420P, and encoded via `libx264` (`preset=ultrafast`, `tune=zerolatency`, forced keyframes aligned to `HLS_SEGMENT_DURATION`).

### 2.2 Unified Live + DVR Storage

`live.m3u8` and `playback.m3u8` reference the exact same `.m4s` segment files per stream directory (`HLS_OUTPUT_DIR` in single-stream mode, `HLS_OUTPUT_DIR/<id>` in multi-stream mode):
- `write_streams_json()` atomically publishes `<HLS_OUTPUT_DIR>/streams.json` listing active streams and their HTTP prefixes (`/hls` or `/hls/<id>`).
- `HlsMuxer` writes `live.m3u8` with `hls_list_size` (`HLS_LIVE_LIST_SIZE`, default 5) and `hls_flags=independent_segments+discont_start+temp_file` (`+append_list` on reconnects).
- `PlaybackManager` periodically parses `live.m3u8`, tracks active live filenames, appends newly completed segments, writes `playback.m3u8` and `events.json` atomically via temp-file + rename, and deletes expired `.m4s` / unused `init_*.mp4` files older than `PLAYBACK_RETENTION_SEC` while protecting any segment still present in `live.m3u8`.
- Nginx maps `location /hls/` with `root /data;` so both `/hls/streams.json` and nested `/hls/<id>/*` paths resolve cleanly without `alias` path-translation issues.

---

## 3. AI Inference & Object Search

### 3.1 Decoupled Ring Buffer & Worker

```text
+-----------------------------------------+        +----------------------------------------+
|          MAIN PIPELINE THREAD           |        |            AI WORKER THREAD            |
|                                         |        |                                        |
|  [ Demux -> Decode -> sws_scale ]       |        |  [ DropOldestRingBuffer (cap=2) ]      |
|       |                        |        |        |                  |                     |
|       | RGB24                  | YUV420P|        |                  v                     |
|       | (ByteBufferPool copy)  |        |        |  [ Letterbox -> ONNX Runtime -> NMS ]  |
|       +====== push ============================> |                  |                     |
|                                |        |        |                  v                     |
|                                v        |        |  [ Update latest_detections_ ]         |
|                     +-------------------+        +------------------+---------------------+
|                     | YUV420P Overlay   | <-------------------------+ reads latest boxes
|                     +---------+---------+                             & logs events.json
|                               |
|                               v
|                     [ libx264 -> HlsMuxer ]
+-----------------------------------------+
```

- **Drop-Oldest Queue (`DropOldestRingBuffer`, capacity 2)**: If ONNX inference runs slower than frame capture, the oldest queued frame is evicted so video capture and HLS segment output never stall.
- **`ByteBufferPool`**: Recycles fixed-size RGB buffers via `std::shared_ptr` custom deleters to avoid repeated heap allocations.
- **ONNX Runtime C++ Execution**: One process-wide `Ort::Env` singleton (`shared_ort_env()`) is shared across concurrent `AiWorker` instances, while each stream owns an independent `Ort::Session` (`ORT_ENABLE_ALL`) so multi-stream inference threads run safely in parallel. Each session dynamically inspects tensor dimensions (e.g., `640x640` or `320x320`), handles both `[1, 84, N]` and `[1, N, 84]` output layouts, and applies confidence filtering + per-class NMS.

### 3.2 Direct YUV420P Overlay

Instead of converting frames to RGB for drawing and back to YUV420P for encoding, `draw_detections_yuv420p` writes 2px solid borders, dark header strips, and a built-in 5x7 bitmap font directly to `Y`, `U`, and `V` planes (subsampled 2x2) with image-bounds clamping.

### 3.3 Object Search & DVR Seek

- `PlaybackManager::record_detections` logs throttled (~1 Hz) detection records mapped to rolling `media_time_sec` and active `.m4s` segment names in `events.json`.
- Nginx serves `events.json`, `stats.json`, `live.m3u8`, and `playback.m3u8` as static files.
- `web/player.js` fetches `/hls/streams.json` on startup (falling back to `/hls/live.m3u8` in single-stream deployments), populates the stream selector dropdown (`1`–`9` keyboard shortcuts), polls per-stream `events.json` and `stats.json`, resets playback/search state on stream switches, filters events client-side (`Any`/`All` tags + confidence threshold), and seeks `playback.m3u8` to the matching `media_time_sec` offset when clicked.

---

## 4. Failure & Reconnect Behavior

| Scenario | Detection | Recovery |
| :--- | :--- | :--- |
| **Upstream camera drop / stall** | `AVIOInterruptCB` aborts blocking I/O after `READ_TIMEOUT_MS` (5000ms default). | Main loop tears down capture/muxer, increments `reconnect_epoch`, calls `PlaybackManager::mark_discontinuity("init_<epoch>.mp4")`, sleeps via jittered `ExponentialBackoff`, and resumes segment numbering monotonically. |
| **Upstream parameter change** | Reconnect opens fresh `AVFormatContext` & decoder/encoder. | Writes new `init_<epoch>.mp4` and emits `#EXT-X-DISCONTINUITY` + `#EXT-X-MAP` into `playback.m3u8`. |
| **HTTP proxy / Nginx restart** | Browser HLS.js receives `NETWORK_ERROR`. | `live_streamer` keeps writing to shared disk uninterrupted; `web/player.js` retries with exponential backoff (1s–10s) and resumes. |
| **Browser buffer stall / decode hiccup** | `<video>` `waiting`/`stalled` or HLS.js `MEDIA_ERROR`. | 8s watchdog nudges to `hls.liveSyncPosition` / restarts load; progressive `recoverMediaError()`, `swapAudioCodec()`, or clean player re-init. |

---

## 5. Multi-Camera Scaling Pattern

Within a single container, `STREAMS` runs `1..N` pipelines concurrently on dedicated `std::thread`s sharing one `Ort::Env`. To scale horizontally across machines:

```text
+----------------------------+     Upload .m4s/.m3u8     +-------------------------+     HTTP/2
| Ingest Tier (StatefulSet)  | ------------------------> | Storage & Origin        | -----------------+
|                            |                           | (NVMe / Object Storage) |                  |
| * Pod 1: Cameras 1..N      |                           +-------------------------+                  v
| * Pod 2: Cameras N..M      |                                                               +---------------+
+-------------+--------------+                                                               | CDN / Nginx   |
              | Async Events                                                                 | Edge Caches   |
              v                                                                              +-------+-------+
+----------------------------+     +-------------------------+     +-------------------+             |
| Event Queue                | --> | Search Index            | --> | Query Service     | ----+       |
| (Redis Streams / Kafka)    |     | (ClickHouse / Postgres) |     | (JSON / HTTP API) |     |       |
+----------------------------+     +-------------------------+     +-------------------+     v       v
                                                                                         [ Viewers 1..K ]
```

1. **Ingest**: Partition camera URLs (`STREAMS`) across workers or Kubernetes pods (`1..N` cameras per worker).
2. **Origin & CDN**: Serve immutable `.m4s` fragments with long-lived `Cache-Control` headers while keeping `.m3u8` playlists short-TTL (`no-cache`).
3. **Search Metadata**: Ship per-camera `events.json` batches to an external time-series store for cross-camera queries.

---

## 6. Architectural Trade-offs

### 6.1 Standard HLS-fMP4 vs. LL-HLS / WebRTC
- **Chosen (HLS-fMP4, 2s segments)**: Works over plain static HTTP proxies and CDNs without custom signaling, tolerates brief upstream drops via `#EXT-X-DISCONTINUITY`, and shares one set of `.m4s` files for both Live and DVR modes at ~4–6s end-to-end latency.
- **Alternative (LL-HLS `EXT-X-PART` / WebRTC)**: Cuts latency below 1s but requires chunked HTTP origin push or stateful ICE/UDP media relays.
