document.addEventListener('DOMContentLoaded', () => {
  const video = document.getElementById('video');
  const playerBanner = document.getElementById('player-banner');
  const bannerText = document.getElementById('banner-text');

  const streamSelect = document.getElementById('stream-select');
  const btnLive = document.getElementById('btn-mode-live');
  const btnPlayback = document.getElementById('btn-mode-playback');
  const btnLiveEdge = document.getElementById('btn-live-edge');

  const statState = document.getElementById('stat-state');
  const statMode = document.getElementById('stat-mode');
  const statLatency = document.getElementById('stat-latency');
  const statFps = document.getElementById('stat-fps');
  const statAi = document.getElementById('stat-ai');
  const statDetections = document.getElementById('stat-detections');
  const statResilience = document.getElementById('stat-resilience');

  const searchInput = document.getElementById('search-input');
  const btnClearSearch = document.getElementById('btn-clear-search');
  const matchBtns = document.querySelectorAll('.match-btn');
  const confSlider = document.getElementById('conf-slider');
  const confVal = document.getElementById('conf-val');
  const searchResultsContainer = document.getElementById('search-results');
  const resultCount = document.getElementById('result-count');

  let streamsList = [];
  let activeStream = null;
  let streamDiscoveryTimer = null;
  let currentMode = 'live';
  let hls = null;
  let hasLoadedManifest = false;
  let reconnectAttempts = 0;
  let backoffDelay = 1000;
  let reconnectTimer = null;
  let lastAutoConnectTriggerMs = 0;
  let stallWatchdogTimer = null;
  let mediaRecoveryTimestamps = [];
  let pendingSeekTime = null;
  let playerSessionTag = Date.now();
  let eventsData = [];
  let matchMode = 'any';
  let minConfidence = parseFloat(confSlider.value) || 0.35;

  function getStreamPrefix(streamObj) {
    return (streamObj && streamObj.hls_prefix)
      ? streamObj.hls_prefix.replace(/\/+$/, '')
      : '/hls';
  }

  function getStreamUrls() {
    const prefix = getStreamPrefix(activeStream);
    return {
      live: `${prefix}/live.m3u8`,
      playback: `${prefix}/playback.m3u8`,
      stats: `${prefix}/stats.json`,
      events: `${prefix}/events.json`
    };
  }

  function populateStreamSelector(streams, selectedId) {
    streamSelect.innerHTML = '';
    streams.forEach((s, idx) => {
      const opt = document.createElement('option');
      opt.value = s.id;
      opt.textContent = `[${idx + 1}] ${s.name || s.id}`;
      streamSelect.appendChild(opt);
    });
    if (selectedId) {
      streamSelect.value = selectedId;
    }
  }

  function selectStream(streamId, forceReload = false, initialSeekSec = null) {
    const found = streamsList.find(s => s.id === streamId);
    if (!found) return;
    if (!forceReload && activeStream && activeStream.id === found.id) return;

    activeStream = found;
    streamSelect.value = found.id;
    pendingSeekTime = initialSeekSec;
    statDetections.innerHTML = '';

    const urls = getStreamUrls();
    initPlayer(currentMode === 'live' ? urls.live : urls.playback, currentMode);
    pollStats();
    pollEvents();
  }

  async function discoverStreams() {
    if (window.location.protocol === 'file:') {
      updateStateBadge('OFFLINE');
      showBanner('Open this dashboard over HTTP (e.g. http://localhost:8999) rather than file:// for HLS playback.');
      return;
    }

    try {
      const response = await fetch('/hls/streams.json', { cache: 'no-store' });
      if (response.ok) {
        const data = await response.json();
        if (data && Array.isArray(data.streams) && data.streams.length > 0) {
          streamsList = data.streams;
          const currentId = activeStream && streamsList.some(s => s.id === activeStream.id)
            ? activeStream.id
            : (data.default_stream && streamsList.some(s => s.id === data.default_stream)
              ? data.default_stream
              : streamsList[0].id);
          populateStreamSelector(streamsList, currentId);
          if (!activeStream || activeStream.id !== currentId) {
            selectStream(currentId, true);
          }
          return;
        }
      }
    } catch (_err) {
      // Retry below or fall back to legacy single-stream layout
    }

    // Legacy single-stream fallback when streams.json is absent
    try {
      const legacyResp = await fetch('/hls/live.m3u8', { cache: 'no-store' });
      if (legacyResp.ok) {
        streamsList = [{ id: 'default', name: 'Primary Stream', hls_prefix: '/hls' }];
        populateStreamSelector(streamsList, 'default');
        if (!activeStream) {
          selectStream('default', true);
        }
        return;
      }
    } catch (_err) {
      // Retry discovery shortly
    }

    if (!activeStream) {
      updateStateBadge('CONNECTING');
      showBanner('Waiting for stream server to initialize...');
      if (streamDiscoveryTimer) clearTimeout(streamDiscoveryTimer);
      streamDiscoveryTimer = setTimeout(discoverStreams, 1500);
    }
  }

  // Set via JS property for Chromium autoplay reliability.
  video.muted = true;
  video.loop = false;

  function appendCacheBust(targetUrl, tag) {
    if (!targetUrl || /[?&]cb=/.test(targetUrl)) return targetUrl;
    const sep = targetUrl.includes('?') ? '&' : '?';
    return `${targetUrl}${sep}cb=${tag}`;
  }

  function buildHlsConfig(mode) {
    const BaseLoader = (typeof Hls !== 'undefined' && Hls.DefaultConfig && Hls.DefaultConfig.loader)
      ? Hls.DefaultConfig.loader
      : null;

    class CacheBustedLoader extends (BaseLoader || Object) {
      load(context, config, callbacks) {
        if (context && context.url) {
          context.url = appendCacheBust(context.url, playerSessionTag);
        }
        return super.load(context, config, callbacks);
      }
    }

    const loaderOption = BaseLoader ? { loader: CacheBustedLoader } : {};

    const manifestLoadPolicy = {
      default: {
        maxTimeToFirstByteMs: 60000,
        maxLoadTimeMs: 60000,
        timeoutRetry: {
          maxNumRetry: 1,
          retryDelayMs: 1000,
          maxRetryDelayMs: 2000
        },
        errorRetry: {
          maxNumRetry: 1,
          retryDelayMs: 1000,
          maxRetryDelayMs: 2000
        }
      }
    };

    if (mode === 'playback') {
      // playback.m3u8 grows dynamically without #EXT-X-ENDLIST. Disable live-edge
      // latency snapping so seeking anywhere along the DVR timeline stays put.
      return {
        ...loaderOption,
        lowLatencyMode: false,
        enableWorker: true,
        startPosition: pendingSeekTime !== null ? pendingSeekTime : 0,
        liveSyncDurationCount: 3,
        liveMaxLatencyDurationCount: Infinity,
        maxLiveSyncPlaybackRate: 1,
        liveDurationInfinity: false,
        backBufferLength: 1800,
        maxBufferLength: 30,
        maxMaxBufferLength: 60,
        capLevelToPlayerSize: true,
        manifestLoadPolicy,
        fragLoadingMaxRetry: 10
      };
    }

    return {
      ...loaderOption,
      lowLatencyMode: false,
      enableWorker: true,
      liveSyncDurationCount: 2,
      liveMaxLatencyDurationCount: 5,
      maxMaxBufferLength: 60,
      capLevelToPlayerSize: true,
      manifestLoadPolicy,
      fragLoadingMaxRetry: 10
    };
  }

  function clearStallWatchdog() {
    if (stallWatchdogTimer) {
      clearTimeout(stallWatchdogTimer);
      stallWatchdogTimer = null;
    }
  }

  function armStallWatchdog() {
    if (stallWatchdogTimer || video.paused) return;
    stallWatchdogTimer = setTimeout(() => {
      stallWatchdogTimer = null;
      if (video.paused || !hls) return;
      console.warn('Playback stalled for 8s, nudging buffer/live sync...');
      if (currentMode === 'live' && hls.liveSyncPosition) {
        video.currentTime = hls.liveSyncPosition;
      }
      hls.startLoad();
    }, 8000);
  }

  video.addEventListener('waiting', armStallWatchdog);
  video.addEventListener('stalled', armStallWatchdog);
  video.addEventListener('playing', clearStallWatchdog);
  video.addEventListener('timeupdate', clearStallWatchdog);
  video.addEventListener('pause', clearStallWatchdog);
  video.addEventListener('ended', clearStallWatchdog);

  function applyClampedDvrSeek(offsetSec) {
    if (video.seekable && video.seekable.length > 0) {
      const base = video.seekable.start(0);
      const end = video.seekable.end(video.seekable.length - 1);
      video.currentTime = Math.max(base, Math.min(base + offsetSec, end));
    } else {
      video.currentTime = Math.max(0, offsetSec);
    }
    video.play().catch(e => console.warn('Play after seek failed:', e));
  }

  function initPlayer(url, mode = currentMode, preserveBackoff = false) {
    if (window.location.protocol === 'file:') {
      updateStateBadge('OFFLINE');
      showBanner('Open this dashboard over HTTP (e.g. http://localhost:8999) rather than file:// for HLS playback.');
      return;
    }

    clearStallWatchdog();
    hasLoadedManifest = false;
    mediaRecoveryTimestamps = [];
    playerSessionTag = Date.now();
    if (!preserveBackoff) {
      reconnectAttempts = 0;
      backoffDelay = 1000;
      hideBanner();
    }

    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    if (hls) {
      hls.destroy();
      hls = null;
    }

    updateStateBadge('CONNECTING');

    if (typeof Hls !== 'undefined' && Hls.isSupported()) {
      hls = new Hls(buildHlsConfig(mode));
      // Attach media before loading source so MSE initializes first.
      hls.on(Hls.Events.MEDIA_ATTACHED, () => {
        if (hls) {
          hls.loadSource(url);
        }
      });
      hls.attachMedia(video);

      hls.on(Hls.Events.MANIFEST_PARSED, () => {
        hasLoadedManifest = true;
        reconnectAttempts = 0;
        backoffDelay = 1000;
        mediaRecoveryTimestamps = [];
        hideBanner();
        updateStateBadge('STREAMING');

        if (mode === 'playback' && pendingSeekTime !== null) {
          const target = pendingSeekTime;
          pendingSeekTime = null;
          applyClampedDvrSeek(target);
        } else {
          video.play().catch(e => console.warn('Autoplay prevented:', e));
        }
      });

      hls.on(Hls.Events.FRAG_BUFFERED, () => {
        hasLoadedManifest = true;
        reconnectAttempts = 0;
        backoffDelay = 1000;
        hideBanner();
        updateStateBadge('STREAMING');
      });

      hls.on(Hls.Events.ERROR, (_event, data) => {
        if (!data.fatal) {
          if (!hasLoadedManifest && data.type === Hls.ErrorTypes.NETWORK_ERROR) {
            updateStateBadge('CONNECTING');
            showBanner('Waiting for stream to start...');
          }
          return;
        }
        switch (data.type) {
          case Hls.ErrorTypes.NETWORK_ERROR: {
            // hls.startLoad() is a no-op before the first manifest loads; re-init to retry manifest fetch.
            const reloadManifest = !hasLoadedManifest ||
              data.details === Hls.ErrorDetails.MANIFEST_LOAD_ERROR ||
              data.details === Hls.ErrorDetails.MANIFEST_LOAD_TIMEOUT ||
              data.details === Hls.ErrorDetails.MANIFEST_PARSING_ERROR;
            console.warn('HLS network error, scheduling retry:', data.details);
            handleNetworkError(reloadManifest);
            break;
          }
          case Hls.ErrorTypes.MEDIA_ERROR: {
            const now = Date.now();
            mediaRecoveryTimestamps = mediaRecoveryTimestamps.filter(ts => (now - ts) < 10000);
            updateStateBadge('RECONNECTING');

            if (mediaRecoveryTimestamps.length >= 2) {
              console.warn('Repeated fatal media errors within 10s, scheduling backoff rebuild:', data.details);
              mediaRecoveryTimestamps = [];
              handleNetworkError(true);
              break;
            }

            if (mediaRecoveryTimestamps.length === 1 && (now - mediaRecoveryTimestamps[0]) < 5000) {
              console.warn('HLS media error persisted within 5s, swapping audio codec & recovering:', data.details);
              hls.swapAudioCodec();
            } else {
              console.warn('HLS media error, recovering:', data.details);
            }
            mediaRecoveryTimestamps.push(now);
            hls.recoverMediaError();
            break;
          }
          default:
            console.error('Unrecoverable HLS error:', data);
            updateStateBadge('OFFLINE');
            hls.destroy();
            hls = null;
            break;
        }
      });
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
      // Native HLS fallback (Safari)
      video.src = appendCacheBust(url, playerSessionTag);
      video.load();
      video.addEventListener('loadedmetadata', () => {
        hasLoadedManifest = true;
        reconnectAttempts = 0;
        backoffDelay = 1000;
        hideBanner();
        updateStateBadge('STREAMING');
        if (mode === 'playback' && pendingSeekTime !== null) {
          const target = pendingSeekTime;
          pendingSeekTime = null;
          applyClampedDvrSeek(target);
        } else {
          video.play().catch(e => console.warn('Autoplay prevented:', e));
        }
      }, { once: true });
      video.addEventListener('error', () => {
        handleNetworkError(true);
      }, { once: true });
    } else {
      updateStateBadge('OFFLINE');
      showBanner('HLS is not supported in this browser.');
    }
  }

  function handleNetworkError(reloadManifest = false) {
    reconnectAttempts++;
    const waitingForBoot = !hasLoadedManifest;
    updateStateBadge(waitingForBoot ? 'CONNECTING' : 'RECONNECTING');

    const sec = Math.max(1, Math.round(backoffDelay / 1000));
    showBanner(
      waitingForBoot
        ? `Waiting for stream — retrying in ${sec}s (Attempt ${reconnectAttempts})...`
        : `Stream connection interrupted — auto-recovering in ${sec}s (Attempt ${reconnectAttempts})...`
    );

    if (reconnectTimer) clearTimeout(reconnectTimer);

    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      if (reloadManifest || !hasLoadedManifest || !hls) {
        const urls = getStreamUrls();
        initPlayer(currentMode === 'live' ? urls.live : urls.playback, currentMode, true);
      } else {
        hls.startLoad();
      }
    }, backoffDelay);

    backoffDelay = waitingForBoot
      ? Math.min(Math.round(backoffDelay * 1.5), 3000)
      : Math.min(backoffDelay * 2, 10000);
  }

  function showBanner(msg) {
    bannerText.textContent = msg;
    playerBanner.classList.remove('hidden');
  }

  function hideBanner() {
    playerBanner.classList.add('hidden');
  }

  function updateStateBadge(state) {
    statState.textContent = state;
    statState.className = 'badge';
    if (state === 'STREAMING') {
      statState.classList.add('badge-streaming');
    } else if (state === 'RECONNECTING' || state === 'CONNECTING') {
      statState.classList.add('badge-reconnecting');
    } else {
      statState.classList.add('badge-offline');
    }
  }

  function switchMode(mode) {
    currentMode = mode;
    const urls = getStreamUrls();
    if (mode === 'live') {
      pendingSeekTime = null;
      btnLive.classList.add('active');
      btnPlayback.classList.remove('active');
      if (activeStream) initPlayer(urls.live, 'live');
    } else {
      btnPlayback.classList.add('active');
      btnLive.classList.remove('active');
      if (activeStream) initPlayer(urls.playback, 'playback');
    }
  }

  streamSelect.addEventListener('change', (e) => {
    if (e.target.value) {
      selectStream(e.target.value, true);
    }
  });

  btnLive.addEventListener('click', () => switchMode('live'));
  btnPlayback.addEventListener('click', () => switchMode('playback'));

  btnLiveEdge.addEventListener('click', () => {
    if (currentMode !== 'live') {
      switchMode('live');
    } else if (hls && hls.liveSyncPosition) {
      video.currentTime = hls.liveSyncPosition;
    } else if (video.seekable && video.seekable.length > 0) {
      video.currentTime = video.seekable.end(video.seekable.length - 1);
    }
  });

  function seekToTime(targetSec, targetStreamId = null) {
    if (targetStreamId && (!activeStream || activeStream.id !== targetStreamId)) {
      currentMode = 'playback';
      btnPlayback.classList.add('active');
      btnLive.classList.remove('active');
      selectStream(targetStreamId, true, targetSec);
      return;
    }

    if (currentMode !== 'playback') {
      pendingSeekTime = targetSec;
      switchMode('playback');
      return;
    }

    if (video.readyState >= 1) {
      applyClampedDvrSeek(targetSec);
    } else {
      pendingSeekTime = targetSec;
    }
  }

  async function pollStats() {
    if (!activeStream) return;
    const requestedId = activeStream.id;
    try {
      const response = await fetch(getStreamUrls().stats, { cache: 'no-store' });
      if (!response.ok) return;
      const stats = await response.json();
      if (!activeStream || activeStream.id !== requestedId) return;

      if (stats.mode) {
        statMode.textContent = stats.mode.toUpperCase();
      }

      if (stats.ingest_fps !== undefined && stats.output_fps !== undefined) {
        statFps.textContent = `${stats.ingest_fps.toFixed(1)} / ${stats.output_fps.toFixed(1)}`;
      }

      if (stats.ai_avg_ms !== undefined && stats.ai_p95_ms !== undefined) {
        statAi.textContent = `${stats.ai_avg_ms.toFixed(1)} ms avg (p95 ${stats.ai_p95_ms.toFixed(1)} ms)`;
      } else {
        statAi.textContent = 'Disabled';
      }

      if (stats.active_detections && Array.isArray(stats.active_detections)) {
        statDetections.innerHTML = stats.active_detections.map(det =>
          `<span class="det-pill">${escapeHtml(det.label)} (${(det.confidence * 100).toFixed(0)}%)</span>`
        ).join('');
      } else {
        statDetections.innerHTML = '';
      }

      const reconCount = stats.reconnect_count ?? reconnectAttempts;
      const dvrDur = stats.dvr_window_sec ? formatDuration(stats.dvr_window_sec) : '0m 0s';
      statResilience.textContent = `${reconCount} reconnects | ${dvrDur} DVR`;

      if (stats.state === 'reconnecting' && currentMode === 'live') {
        updateStateBadge('RECONNECTING');
      } else if (!hasLoadedManifest && stats.state === 'streaming' && stats.active_live_segments > 0) {
        const now = Date.now();
        if ((now - lastAutoConnectTriggerMs) >= 1500) {
          lastAutoConnectTriggerMs = now;
          if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
          }
          const urls = getStreamUrls();
          initPlayer(currentMode === 'live' ? urls.live : urls.playback, currentMode, true);
        }
      }
    } catch (_err) {
      // Ignore transient telemetry fetch errors during network interruptions
    }
  }

  setInterval(() => {
    if (currentMode === 'playback') {
      if (video.seekable && video.seekable.length > 0) {
        const start = video.seekable.start(0);
        const end = video.seekable.end(video.seekable.length - 1);
        const pos = Math.max(0, video.currentTime - start);
        const total = Math.max(0, end - start);
        statLatency.textContent = `DVR ${formatMinutesSeconds(pos)} / ${formatMinutesSeconds(total)}`;
      } else {
        statLatency.textContent = 'DVR';
      }
    } else if (hls && hls.latency !== undefined) {
      statLatency.textContent = `${hls.latency.toFixed(1)}s`;
    } else if (video.seekable && video.seekable.length > 0) {
      const latency = video.seekable.end(video.seekable.length - 1) - video.currentTime;
      statLatency.textContent = `${Math.max(0, latency).toFixed(1)}s`;
    } else {
      statLatency.textContent = '--';
    }
  }, 500);

  setInterval(pollStats, 1000);

  async function pollEvents() {
    const targets = streamsList.length > 0
      ? streamsList
      : (activeStream ? [activeStream] : []);
    if (targets.length === 0) return;

    const merged = [];
    let anySucceeded = false;

    await Promise.all(targets.map(async (s) => {
      try {
        const prefix = getStreamPrefix(s);
        const response = await fetch(`${prefix}/events.json`, { cache: 'no-store' });
        if (!response.ok) return;
        const data = await response.json();
        const list = Array.isArray(data) ? data : (Array.isArray(data.events) ? data.events : null);
        if (!list) return;
        anySucceeded = true;
        const streamLabel = s.name || s.id;
        list.forEach(ev => {
          merged.push({
            ...ev,
            _streamId: s.id,
            _streamName: streamLabel
          });
        });
      } catch (_err) {
        // Ignore transient per-stream event index fetch errors
      }
    }));

    if (anySucceeded) {
      eventsData = merged;
      renderSearchResults();
    }
  }

  setInterval(pollEvents, 2000);

  searchInput.addEventListener('input', renderSearchResults);
  btnClearSearch.addEventListener('click', () => {
    searchInput.value = '';
    renderSearchResults();
  });

  matchBtns.forEach(btn => {
    btn.addEventListener('click', () => {
      matchBtns.forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      matchMode = btn.getAttribute('data-match');
      renderSearchResults();
    });
  });

  confSlider.addEventListener('input', () => {
    minConfidence = parseFloat(confSlider.value);
    confVal.textContent = minConfidence.toFixed(2);
    renderSearchResults();
  });

  function renderSearchResults() {
    const rawQuery = searchInput.value.trim();
    const queryTokens = rawQuery
      ? rawQuery.split(',').map(t => t.trim().toLowerCase()).filter(t => t.length > 0)
      : [];

    const filtered = eventsData.filter(ev => {
      if (!ev.detections || !Array.isArray(ev.detections)) return false;

      const validDetections = ev.detections.filter(d => (d.confidence || 0) >= minConfidence);
      if (validDetections.length === 0) return false;

      if (queryTokens.length === 0) return true;

      const detectedLabels = validDetections.map(d => (d.label || '').toLowerCase());
      if (matchMode === 'all') {
        return queryTokens.every(token => detectedLabels.some(label => label.includes(token)));
      }
      return queryTokens.some(token => detectedLabels.some(label => label.includes(token)));
    });

    filtered.sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
    const cappedResults = filtered.slice(0, 100);

    resultCount.textContent = `${cappedResults.length} results`;

    if (cappedResults.length === 0) {
      searchResultsContainer.innerHTML = `
        <div class="empty-state">
          <p>No matching detection events found.</p>
          <p class="empty-sub">Try adjusting search terms or lowering confidence threshold.</p>
        </div>
      `;
      return;
    }

    searchResultsContainer.innerHTML = cappedResults.map(ev => {
      const timeStr = ev.timestamp ? new Date(ev.timestamp * 1000).toLocaleTimeString() : 'Unknown Time';
      const dvrOffsetSec = ev.media_time_sec || 0;
      const dvrOffsetFormatted = formatMinutesSeconds(dvrOffsetSec);
      const segmentName = ev.segment_name || 'segment.m4s';

      const validDetections = (ev.detections || []).filter(d => (d.confidence || 0) >= minConfidence);
      const chipsHtml = validDetections.map(d => {
        const labelLower = (d.label || '').toLowerCase();
        const isHighlight = queryTokens.length > 0 && queryTokens.some(t => labelLower.includes(t));
        const confPercent = Math.round((d.confidence || 0) * 100);
        return `
          <span class="object-chip ${isHighlight ? 'highlight' : ''}">
            ${escapeHtml(d.label)} <span class="object-conf">${confPercent}%</span>
          </span>
        `;
      }).join('');

      const streamId = ev._streamId || (activeStream ? activeStream.id : '');
      const streamLabel = ev._streamName || streamId || 'stream';

      return `
        <div class="result-item" data-seek="${dvrOffsetSec}" data-stream-id="${escapeHtml(streamId)}">
          <div class="result-meta-row">
            <div class="result-meta-left">
              <span class="result-stream-badge">${escapeHtml(streamLabel)}</span>
              <span class="result-time">${timeStr}</span>
            </div>
            <span class="result-dvr-offset">DVR offset: ${dvrOffsetFormatted}</span>
          </div>
          <div class="result-segment">${escapeHtml(segmentName)}</div>
          <div class="result-objects-row">
            ${chipsHtml}
          </div>
        </div>
      `;
    }).join('');

    searchResultsContainer.querySelectorAll('.result-item').forEach(item => {
      item.addEventListener('click', () => {
        const seekSec = parseFloat(item.getAttribute('data-seek'));
        const targetStreamId = item.getAttribute('data-stream-id');
        if (!isNaN(seekSec)) {
          seekToTime(seekSec, targetStreamId);
        }
      });
    });
  }

  function formatDuration(totalSec) {
    const mins = Math.floor(totalSec / 60);
    const secs = Math.floor(totalSec % 60);
    return `${mins}m ${secs}s`;
  }

  function formatMinutesSeconds(totalSec) {
    const mins = Math.floor(totalSec / 60);
    const secs = Math.floor(totalSec % 60);
    return `${String(mins).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
  }

  function escapeHtml(str) {
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#039;');
  }

  document.addEventListener('keydown', (event) => {
    if (event.metaKey || event.ctrlKey || event.altKey) return;
    const activeTag = document.activeElement ? document.activeElement.tagName : '';
    if (activeTag === 'INPUT' || activeTag === 'TEXTAREA' || activeTag === 'SELECT') return;

    const key = event.key;
    if (key === ' ' || key === 'k' || key === 'K') {
      event.preventDefault();
      if (video.paused) {
        video.play().catch(() => {});
      } else {
        video.pause();
      }
    } else if (key === 'm' || key === 'M') {
      video.muted = !video.muted;
    } else if (key === 'f' || key === 'F') {
      const container = video.closest('.video-container') || video;
      if (document.fullscreenElement) {
        document.exitFullscreen().catch(() => {});
      } else if (container.requestFullscreen) {
        container.requestFullscreen().catch(() => {});
      }
    } else if (key === 'l' || key === 'L') {
      btnLiveEdge.click();
    } else if (key >= '1' && key <= '9') {
      const idx = parseInt(key, 10) - 1;
      if (idx < streamsList.length) {
        selectStream(streamsList[idx].id, true);
      }
    } else if ((key === 'ArrowLeft' || key === 'ArrowRight') && currentMode === 'playback') {
      event.preventDefault();
      const delta = key === 'ArrowLeft' ? -5 : 5;
      if (video.seekable && video.seekable.length > 0) {
        const minTime = video.seekable.start(0);
        const maxTime = video.seekable.end(video.seekable.length - 1);
        video.currentTime = Math.max(minTime, Math.min(video.currentTime + delta, maxTime));
      } else {
        video.currentTime = Math.max(0, video.currentTime + delta);
      }
    }
  });

  discoverStreams();
});
