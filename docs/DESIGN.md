# Live Video Streamer — System Design

## 1. Overview

This document describes the architecture of `live_streamer`, a C++20 Libav (`libavformat` / `libavcodec` / `libavdevice` / `libswscale`) streaming and local AI inference engine packaged in a static-HTTP HLS-fMP4 layout.

- **Direct Libav Pipeline**: The core binary handles stream ingest, demuxing, decoding, encoding, and HLS-fMP4 muxing directly through Libav C APIs without an intermediary media server. (`docker-compose.yml` uses MediaMTX only as an optional test camera simulator pushing RTSP.)
- **HLS-fMP4 Output**: Writes `init_<epoch>.mp4` + `.m4s` fragments (`HLS_SEGMENT_DURATION=2s` by default), paired with rolling `live.m3u8` and DVR `playback.m3u8` playlists.
- **Direct YUV420P AI Overlay**: Runs YOLOv8 ONNX Runtime inference on a background thread and draws boxes/labels directly onto decoded YUV420P frames without a second RGB conversion.
- **Two-Sided Outage Handling**: Combines `AVIOInterruptCB` deadlines and jittered exponential backoff in C++ with retry/stall watchdogs in `web/player.js`.

---

## 2. Streaming & Packaging Pipeline

### 2.1 Smart Pass-Through vs. Conditional Transcoding

```text
+--------------------------------+
| RTSP / Camera Stream Ingest    |
+---------------+----------------+
                |
                v
         [ AI Enabled? ]
          /           \
       (No)           (Yes)
        /               \
       v                 v
[ Codec H.264/HEVC? ]   [ Decode to AVFrame (YUV420P) ]
     /        \                   |
  (Yes)       (No)                v
   /            \       [ ONNX Runtime YOLOv8n Inference ]
  v              |                |
[ Smart Remux ]  |                v
[ (Zero-Copy) ]  |      [ YUV420P Direct Bitmap Overlay ]
  |              |                |
  |              +-------+--------+
  |                      |
  |                      v
  |              [ libx264 Transcode ]
  |              [ (preset=ultrafast) ]
  |                      |
  +----------+-----------+
             |
             v
    +------------------+
    | HLS-fMP4 Muxer   |
    +------------------+
```

1. **Smart Remux (`AI_ENABLED=false` and compatible H.264/HEVC input under `TRANSCODE_MODE=auto` or `remux`)**: Demuxed packets are rescaled and written directly to HLS-fMP4 without decoding or re-encoding.
2. **Transcode Path (`AI_ENABLED=true`, `TRANSCODE_MODE=always`, or non-H.264/HEVC source)**: Decoded frames are converted via `libswscale` to YUV420P (and RGB24 for AI when enabled), annotated directly in YUV420P, and encoded via `libx264` (`preset=ultrafast`, `tune=zerolatency`, forced keyframes aligned to `HLS_SEGMENT_DURATION`).

### 2.2 Unified Live + DVR Storage

`live.m3u8` and `playback.m3u8` reference the exact same `.m4s` segment files in `HLS_OUTPUT_DIR`:
- `HlsMuxer` writes `live.m3u8` with `hls_list_size` (`HLS_LIVE_LIST_SIZE`, default 5) and `hls_flags=independent_segments+discont_start+temp_file` (`+append_list` on reconnects).
- `PlaybackManager` periodically parses `live.m3u8`, tracks active live filenames, appends newly completed segments, writes `playback.m3u8` and `events.json` atomically via temp-file + rename, and deletes expired `.m4s` / unused `init_*.mp4` files older than `PLAYBACK_RETENTION_SEC` while protecting any segment still present in `live.m3u8`.

---

## 3. AI Inference & Object Search

### 3.1 Decoupled Ring Buffer & Worker

```text
+-----------------------------------+      +----------------------------------------------+
|          INGEST THREAD            |      |               AI WORKER THREAD             |
|                                   |      |                                              |
| [ AVPacket Capture & Decode ]     |      | [ ByteBufferPool ] -> [ ONNX YOLOv8n ]       |
|               |                   |      |                             |                |
|               v                   |      |                             v                |
|  (Push RGB/YUV Frame)             |      |                  (Bounding Boxes + Labels)   |
|               |                   |      |                             |                |
|               +---> [ Drop-Oldest RingBuffer (cap=2) ] ----------------+                |
|                                   |      |                             v                |
|                                   |      |                  [ YUV420P Direct Overlay ]  |
+-----------------------------------+      +----------------------------------------------+
```

- **Drop-Oldest Queue (`DropOldestRingBuffer`, capacity 2)**: If ONNX inference runs slower than frame capture, the oldest queued frame is evicted so video capture and HLS segment output never stall.
- **`ByteBufferPool`**: Recycles fixed-size RGB buffers via `std::shared_ptr` custom deleters to avoid repeated heap allocations.
- **ONNX Runtime C++ Execution**: Runs on CPU (`Ort::Session` with `ORT_ENABLE_ALL`), dynamically inspects tensor dimensions (e.g., `640x640` or `320x320`), handles both `[1, 84, N]` and `[1, N, 84]` output layouts, and applies confidence filtering + per-class NMS.

### 3.2 Direct YUV420P Overlay

Instead of converting frames to RGB for drawing and back to YUV420P for encoding, `draw_detections_yuv420p` writes 2px solid borders, dark header strips, and a built-in 5x7 bitmap font directly to `Y`, `U`, and `V` planes (subsampled 2x2) with image-bounds clamping.

### 3.3 Object Search & DVR Seek

- `PlaybackManager::record_detections` logs throttled (~1 Hz) detection records mapped to rolling `media_time_sec` and active `.m4s` segment names in `events.json`.
- Nginx serves `events.json`, `stats.json`, `live.m3u8`, and `playback.m3u8` as static files.
- `web/player.js` polls `events.json` and `stats.json`, filters events client-side (`Any`/`All` tags + confidence threshold), and seeks `playback.m3u8` to the matching `media_time_sec` offset when clicked.

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

To scale beyond a single instance:

```text
+----------------------------+     Upload .m4s/.m3u8     +-------------------------+     HTTP/2
| Ingest Tier (StatefulSet)  | ------------------------> | Storage & Origin        | ------------+
|                            |                           | (NVMe / Object Storage) |             |
| * Pod 1: Cameras 1..N      |                           +-------------------------+             v
| * Pod 2: Cameras N..M      |                                                           +---------------+
+-------------+--------------+                                                           | CDN / Nginx   |
              | Async Events                                                             | Edge Caches   |
              v                                                                          +-------+-------+
+----------------------------+     +-------------------------+     +-------------------+         |
| Event Queue                | --> | Search Index            | --> | Query Service     |         v
| (Redis Streams / Kafka)    |     | (ClickHouse / Postgres) |     |                   |   [ Viewers 1..K ]
+----------------------------+     +-------------------------+     +-------------------+
```

1. **Ingest**: Partition camera URLs across stateless workers or Kubernetes pods (`1..N` cameras per worker).
2. **Origin & CDN**: Serve immutable `.m4s` fragments with long-lived `Cache-Control` headers while keeping `.m3u8` playlists short-TTL (`no-cache`).
3. **Search Metadata**: Ship per-camera `events.json` batches to an external time-series store for cross-camera queries.

---

## 6. Architectural Trade-offs

### 6.1 Standard HLS-fMP4 vs. LL-HLS / WebRTC
- **Chosen (HLS-fMP4, 2s segments)**: Works over plain static HTTP proxies and CDNs without custom signaling, tolerates brief upstream drops via `#EXT-X-DISCONTINUITY`, and shares one set of `.m4s` files for both Live and DVR modes at ~4–6s end-to-end latency.
- **Alternative (LL-HLS `EXT-X-PART` / WebRTC)**: Cuts latency below 1s but requires chunked HTTP origin push or stateful ICE/UDP media relays.
