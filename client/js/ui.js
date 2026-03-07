
import { CODECS, CODEC_KEYS } from './constants.js';
import { S, $, detectCodecs, subscribeToMetrics, safe, log, bus } from './state.js';
import { toggleAudio } from './media.js';
import { setRelativeMouseMode } from './input.js';
import { toggleMic, isMicSupported } from './mic.js';
import { applyFps, sendMonitor, applyCodec } from './protocol.js';
const loadEl = $('loadingOverlay');
const statusEl = $('loadingStatus');
const subStatusEl = $('loadingSubstatus');

export const updateLoadingStage = (status, subStatus = '') => {
    statusEl.textContent = S.isReconnecting ? 'Reconnecting...' : status;
    subStatusEl.textContent = subStatus;
    loadEl.classList.toggle('reconnecting', S.isReconnecting);
};

export const showLoading = (isReconnect = false) => {
    S.isReconnecting = isReconnect;
    S.firstFrameReceived = 0;
    loadEl.classList.remove('hidden');
    updateLoadingStage('Connecting...', 'Initializing');
    log.debug('UI', 'Loading shown', { isReconnect });
};

export const hideLoading = () => {
    S.firstFrameReceived = 1;
    updateLoadingStage('Connected');
    setTimeout(() => loadEl.classList.add('hidden'), 300);
    log.debug('UI', 'Loading hidden');
};
bus.on('fps:applied', fps => updateFpsDropdown(fps));
const fpsSel = $('fpsSel');
const monSel = $('monSel');
const codecSel = $('codecSel');
const customFpsRow = $('customFpsRow');
const customFpsInput = $('customFpsInput');
const customFpsApply = $('customFpsApply');
const STORAGE_KEYS = {
    FPS: 'slipstream_fps',
    CODEC: 'slipstream_codec',
    TABBED: 'slipstream_tabbed_mode',
    STATS: 'slipstream_stats_overlay',
    CLIPBOARD: 'slipstream_clipboard_sync',
    MON_NAMES: 'slipstream_monitor_names'
};
const loadPref = (key, validator, defaultVal = null) => {
    try {
        const val = parseInt(localStorage.getItem(key));
        return validator(val) ? val : defaultVal;
    } catch (e) {
        log.warn('UI', 'Failed to load preference', { key, error: e.message });
        return defaultVal;
    }
};

const savePref = (key, val) => {
    safe(() => localStorage.setItem(key, val.toString()), undefined, 'UI');
};

const loadBoolPref = key => safe(() => localStorage.getItem(key) === 'true', false, 'UI');
const loadJsonPref = (key, fallback = {}) => safe(() => JSON.parse(localStorage.getItem(key)) || fallback, fallback, 'UI');
const saveJsonPref = (key, value) => safe(() => localStorage.setItem(key, JSON.stringify(value)), undefined, 'UI');
const setDisplay = (el, visible, display = 'flex') => {
    el.style.display = visible ? display : 'none';
};
const applySavedFps = (fps, source) => {
    savePref(STORAGE_KEYS.FPS, fps);
    applyFps(fps);
    log.info('UI', source, { fps });
};
const bindNumericSelect = (el, apply, logMessage, storageKey = null, dataKey = 'value') => {
    el.onchange = () => {
        const value = +el.value;
        if (storageKey) savePref(storageKey, value);
        apply(value);
        log.info('UI', logMessage, { [dataKey]: value });
    };
};
const updateDropSection = (sectionId, hasDrops, values) => {
    const section = $(sectionId);
    if (!section) return;
    section.classList.toggle('has-drops', hasDrops);
    if (!hasDrops) return;
    for (const [id, value] of Object.entries(values)) {
        const el = $(id);
        if (el) el.textContent = value;
    }
};

export const getStoredFps = () => loadPref(STORAGE_KEYS.FPS, v => v >= 1 && v <= 240, null);
export const getStoredCodec = () => loadPref(STORAGE_KEYS.CODEC, v => v >= 0 && v <= 2, null) ?? defaultCodec ?? 0;
const togglePanel = show => {
    ['panel', 'backdrop', 'edge'].forEach(id => $(id).classList.toggle('on', show));
    log.debug('UI', 'Panel toggled', { show });
};

$('edge').onclick = () => togglePanel(true);
$('panelClose').onclick = $('backdrop').onclick = () => togglePanel(false);

document.onkeydown = e => {
    if (e.key === 'Escape' && $('panel').classList.contains('on') && !S.controlEnabled) {
        togglePanel(false);
    }
};
export const updateFpsDropdown = fps => {
    const value = +fps;
    const standardValues = ['15', '30', '60', '120', '144'];
    const isCustom = !standardValues.includes(value.toString());

    if (!isCustom) {
        fpsSel.value = value.toString();
    } else {
        fpsSel.value = 'custom';
        customFpsInput.value = value;
    }
    setDisplay(customFpsRow, isCustom);
};

fpsSel.onchange = () => {
    if (fpsSel.value === 'custom') {
        setDisplay(customFpsRow, true);
    } else {
        setDisplay(customFpsRow, false);
        applySavedFps(+fpsSel.value, 'FPS changed');
    }
};

customFpsApply.onclick = () => {
    const fps = parseInt(customFpsInput.value, 10);
    if (fps >= 1 && fps <= 240) {
        applySavedFps(fps, 'Custom FPS applied');
    } else {
        log.warn('UI', 'Invalid FPS value', { fps });
    }
};

customFpsInput.onkeydown = e => {
    if (e.key === 'Enter') { e.preventDefault(); customFpsApply.click(); }
};
export const updateCodecDropdown = codecId => { if (codecSel.value !== codecId.toString()) codecSel.value = codecId.toString(); };

export const setHostCodecs = caps => { S.hostCodecs = caps; updateCodecOpts(); log.debug('UI', 'Host codecs set', { caps: caps.toString(2) }); };

let defaultCodec = null;

export const initCodecDetection = async () => {
    try {
        const { best } = await detectCodecs();
        defaultCodec = best.codecId;
        log.info('UI', 'Codec detection complete', { best: best.codecName });
        return best;
    } catch (e) {
        log.error('UI', 'Codec detection failed', { error: e.message });
        defaultCodec = 1;
        return { codecId: 1, hardwareAccel: 0, codecName: 'H.265' };
    }
};

export const updateCodecOpts = async () => {
    const { support } = await detectCodecs();
    const hostCaps = S.hostCodecs;

    const supportedCodecs = Object.values(CODECS).filter(codec => {
        const key = CODEC_KEYS[codec.id];
        return support[key] && (hostCaps & (1 << codec.id));
    });

    codecSel.innerHTML = supportedCodecs.length
        ? supportedCodecs.map(codec => `<option value="${codec.id}">${codec.name}</option>`).join('')
        : '<option value="2">H.264</option>';

    const storedCodec = loadPref(STORAGE_KEYS.CODEC, v => v >= 0 && v <= 2, null);
    const preferred = storedCodec ?? defaultCodec ?? 0;
    const canUse = support[CODEC_KEYS[preferred]] && (hostCaps & (1 << preferred));

    if (canUse) {
        codecSel.value = preferred;
    } else {
        const fallback = [0, 1, 2].find(id => support[CODEC_KEYS[id]] && (hostCaps & (1 << id)));
        codecSel.value = fallback ?? 2;
        log.warn('UI', 'Preferred codec unavailable, using fallback', { preferred, fallback });
    }

    S.currentCodec = +codecSel.value;
};

bindNumericSelect(codecSel, applyCodec, 'Codec changed', STORAGE_KEYS.CODEC, 'codec');
bindNumericSelect(monSel, sendMonitor, 'Monitor changed', null, 'index');
$('aBtn').onclick = toggleAudio;
const micBtn = $('micBtn');
const micTxt = $('micTxt');
const micHint = $('micHint');

const updateMicUI = () => {
    if (!micBtn) return;
    micBtn.classList.toggle('on', S.micEnabled);
    micTxt.textContent = S.micEnabled ? 'Disable' : 'Enable';
    micHint?.classList.toggle('visible', !isMicSupported());
};

if (micBtn) {
    micBtn.onclick = async () => { await toggleMic(); updateMicUI(); };

    if (micHint && !isMicSupported()) {
        micHint.textContent = 'Mic not supported';
        micHint.classList.add('visible');
    }
}
const fsPath = $('fsp');
const fsTxt = $('fst');
const FS_PATHS = [
    'M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3',
    'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'
];

const fsIcon = $('fsi');
fsIcon.setAttribute('fill', 'none');
fsIcon.setAttribute('stroke', 'currentColor');
fsIcon.setAttribute('stroke-width', '2');

const updateFullscreen = () => {
    const isFs = !!document.fullscreenElement;
    fsPath.setAttribute('d', FS_PATHS[isFs ? 1 : 0]);
    fsTxt.textContent = isFs ? 'Exit Fullscreen' : 'Fullscreen';
};

updateFullscreen();

$('fs').onclick = async () => {
    if (document.fullscreenElement) {
        document.exitFullscreen();
    } else {
        try {
            await document.documentElement.requestFullscreen?.();
        } catch (e) {
            log.warn('UI', 'Fullscreen request failed', { error: e.message });
        }
    }
};

const hasKeyboardLock = 'keyboard' in navigator && 'lock' in navigator.keyboard;

document.addEventListener('fullscreenchange', async () => {
    updateFullscreen();

    if (!hasKeyboardLock) {
        S.keyboardLockActive = 0;
        return;
    }

    if (document.fullscreenElement) {
        let lockOk = false;
        try {
            await navigator.keyboard.lock();
            lockOk = true;
        } catch {
            const escLocked = await safe(() => navigator.keyboard.lock(['Escape']), undefined, 'UI');
            lockOk = escLocked !== undefined;
        }
        if (lockOk) {
            S.keyboardLockActive = 1;
        } else {
            S.keyboardLockActive = 0;
            log.warn('UI', 'Keyboard lock unavailable; system keys may affect local OS');
        }
    } else {
        safe(() => navigator.keyboard.unlock(), undefined, 'UI');
        S.keyboardLockActive = 0;
    }
});

window.addEventListener('blur', () => {
    if (!document.fullscreenElement) S.keyboardLockActive = 0;
});
const tabStrip = $('tabStrip');
const tabContainer = $('tabContainer');
const tabbedModeBtn = $('tabbedModeBtn');
const tabBack = $('tabBack');
const monitorNames = loadJsonPref(STORAGE_KEYS.MON_NAMES);

const MONITOR_ICON = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>`;
const STAR_ICON = `<svg class="primary-star" viewBox="0 0 24 24" fill="currentColor"><path d="M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z"/></svg>`;

const setMonitorName = (idx, name) => {
    if (name && name.trim()) {
        monitorNames[idx] = name.trim();
    } else {
        delete monitorNames[idx];
    }
    saveJsonPref(STORAGE_KEYS.MON_NAMES, monitorNames);
};

const syncTabbedVisibility = () => {
    const show = S.tabbedMode && S.monitors.length > 0;
    tabStrip.classList.toggle('visible', show);
    if (show) renderTabs();
};

const getMonitorDisplayName = m => monitorNames[m.index] || m.name || `Display ${m.index + 1}`;

const renderTabs = () => {
    if (!S.monitors.length) {
        tabContainer.innerHTML = '';
        return;
    }

    tabContainer.innerHTML = S.monitors.map(m => `
        <button class="tab-item${m.index === S.currentMon ? ' active' : ''}" data-index="${m.index}">
            ${MONITOR_ICON}
            <div class="tab-item-info">
                <span class="tab-item-name">${getMonitorDisplayName(m)}${m.isPrimary ? STAR_ICON : ''}</span>
            </div>
        </button>
    `).join('');

    tabContainer.querySelectorAll('.tab-item').forEach(tab => {
        const idx = +tab.dataset.index;

        tab.onclick = () => {
            if (idx !== S.currentMon) {
                sendMonitor(idx);
            }
        };

        tab.ondblclick = e => {
            if (idx !== S.currentMon) return;
            e.preventDefault();

            const nameSpan = tab.querySelector('.tab-item-name');
            const monitor = S.monitors.find(m => m.index === idx);
            if (!nameSpan || !monitor) return;

            const currentName = getMonitorDisplayName(monitor);
            const star = nameSpan.querySelector('.primary-star');

            const input = document.createElement('input');
            input.type = 'text';
            input.className = 'tab-name-input';
            input.value = currentName;
            input.maxLength = 32;

            const resize = () => {
                input.style.width = Math.min(150, Math.max(20, (input.value.length + 1) * 8)) + 'px';
            };

            nameSpan.textContent = '';
            nameSpan.appendChild(input);
            if (star) nameSpan.appendChild(star);
            resize();
            input.focus();
            input.select();

            input.oninput = resize;

            const save = () => {
                const newName = input.value.trim();
                setMonitorName(idx, newName);
                renderTabs();
                log.info('UI', 'Monitor renamed', { index: idx, name: newName || '(default)' });
            };

            input.onblur = save;
            input.onkeydown = ev => {
                if (ev.key === 'Enter') { ev.preventDefault(); input.blur(); }
                else if (ev.key === 'Escape') { ev.preventDefault(); input.value = ''; input.blur(); }
            };
            input.onclick = ev => ev.stopPropagation();
        };
    });
};

const updateTabbedUI = () => {
    tabbedModeBtn.classList.toggle('on', S.tabbedMode);
    document.body.classList.toggle('tabbed-mode', S.tabbedMode);
    syncTabbedVisibility();

    setTimeout(() => window.dispatchEvent(new Event('resize')), 350);
};

export const closeTabbedMode = () => { if (setTabbedMode(false)) log.info('UI', 'Tabbed mode closed'); };

const setTabbedMode = enabled => {
    if (S.tabbedMode === enabled) return false;
    S.tabbedMode = enabled;
    savePref(STORAGE_KEYS.TABBED, enabled);
    updateTabbedUI();
    return true;
};

S.tabbedMode = loadBoolPref(STORAGE_KEYS.TABBED);

tabbedModeBtn.onclick = () => {
    const enabled = !S.tabbedMode;
    setTabbedMode(enabled);
    log.info('UI', 'Tabbed mode toggled', { enabled });
};

tabBack.onclick = closeTabbedMode;

updateTabbedUI();

export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length
        ? S.monitors.map(m => `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`).join('')
        : '<option value="0">Waiting...</option>';
    monSel.value = S.currentMon;

    if (S.tabbedMode) syncTabbedVisibility();

    log.debug('UI', 'Monitor options updated', { count: S.monitors.length });
};
const STATS_SCHEMA = [
    { label: 'THROUGHPUT', rows: [['FPS', 'statsFps'], ['Bitrate', 'statsBitrate'], ['Resolution', 'statsResolution'], ['Codec', 'statsCodec'], ['Encode', 'statsEncode']] },
    { label: 'LATENCY', rows: [['RTT', 'statsRtt'], ['Video E2E', 'statsE2e'], ['Sync +/-', 'statsSync']] },
    { label: 'E2E STAGES', rows: [['Src->Cap', 'statsStageSourceCapture'], ['Cap->Enc', 'statsStageCaptureEncode'], ['Enc->Send', 'statsStageEncodeSend'], ['Send->Recv', 'statsStageSendReceive'], ['Rx->Asm', 'statsStageReceiveAssemble'], ['Asm->Dec', 'statsStageAssembleDecode'], ['Dec->Present', 'statsStageDecodePresent']] },
    { label: 'JITTER', rows: [['Interval', 'statsInterval'], ['Std Dev', 'statsStdDev']] },
    { label: 'DECODE', rows: [['Time', 'statsDecodeTime'], ['Queue', 'statsDecodeQueue'], ['HW Accel', 'statsHwAccel']] },
    { label: 'RENDER', rows: [['Time', 'statsRenderTime'], ['Frames', 'statsRenderFrames']] },
    { label: 'NETWORK', rows: [['Packets', 'statsPackets'], ['Avg Size', 'statsPacketSize']] },
    { id: 'statsDropsSection', label: 'DROPS', warn: true, rows: [['Dropped', 'statsDropsDropped'], ['Timeout', 'statsDropsTimeout'], ['Late', 'statsDropsLate'], ['Decode Err', 'statsDecodeErrors']] },
    { label: 'AUDIO', rows: [['Packets', 'statsAudioPackets'], ['Decoded', 'statsAudioDecoded'], ['Buffer', 'statsAudioBuffer']] },
    { id: 'statsAudioDropsSection', label: 'AUDIO DROPS', warn: true, rows: [['Dropped', 'statsAudioDropped'], ['Underruns', 'statsAudioUnderruns'], ['Overflows', 'statsAudioOverflows']] },
    { label: 'INPUT', rows: [['Mouse', 'statsInputMouse'], ['Keys', 'statsInputKeys']] },
    { label: 'SESSION', rows: [['Uptime', 'statsUptime'], ['Total Frames', 'statsTotalFrames'], ['Total Data', 'statsTotalData']] },
    { label: 'VERSION', rows: [['Host', 'statsHostVersion']] }
];

const initStatsOverlay = () => {
    const container = $('statsContent');
    if (!container) return;

    container.innerHTML = STATS_SCHEMA.map(section =>
        `<div class="stats-section"${section.id ? ` id="${section.id}"` : ''}>` +
        `<div class="stats-label${section.warn ? ' stats-label-warn' : ''}">${section.label}</div>` +
        section.rows.map(([label, id]) =>
            `<div class="stats-row"><span>${label}</span><span id="${id}">--</span></div>`
        ).join('') +
        `</div>`
    ).join('');
};

initStatsOverlay();

const statsOverlay = $('statsOverlay');
const statsToggle = $('statsToggleBtn');
let statsEnabled = loadBoolPref(STORAGE_KEYS.STATS);
let metricsUnsubscribe = null;

const formatValue = (val, decimals = 2, suffix = '') =>
    Number.isFinite(val) && val >= 0 ? `${val.toFixed(decimals)}${suffix}` : `--${suffix}`;

const formatUptime = totalSeconds => {
    const sec = Math.max(0, Math.floor(totalSeconds || 0));
    const days = Math.floor(sec / 86400);
    const hours = Math.floor((sec % 86400) / 3600);
    const minutes = Math.floor((sec % 3600) / 60);
    const seconds = sec % 60;

    if (days > 0) return `${days}d ${hours}h ${minutes}m ${seconds}s`;
    if (hours > 0) return `${hours}h ${minutes}m ${seconds}s`;
    if (minutes > 0) return `${minutes}m ${seconds}s`;
    return `${seconds}s`;
};

const CODEC_NAMES = ['AV1', 'H.265', 'H.264'];
const getHostEncoderDisplay = () => S.hostEncoderName || '--';

const updateStats = data => {
    if (!statsEnabled) return;

    const { stats, jitter, clock, network, decode, render, audio, session, computed } = data;

    const updates = {
        statsFps: `${computed.fps}/${computed.targetFps} (${computed.fpsEff.toFixed(0)}%)`,
        statsBitrate: `${computed.mbps} Mbps`,
        statsResolution: S.W > 0 ? `${S.W}x${S.H}` : '--x--',
        statsCodec: `${CODEC_NAMES[S.currentCodec] || 'H.264'}`,
        statsEncode: getHostEncoderDisplay(),
        statsRtt: clock.valid ? formatValue(clock.rttMs, 1, ' ms') : '-- ms',
        statsE2e: formatValue(jitter.avgE2eLatencyMs, 1, ' ms'),
        statsSync: clock.valid ? formatValue(clock.uncertaintyMs, 1, ' ms') : '-- ms',
        statsStageSourceCapture: formatValue(jitter.avgSourceCaptureMs, 1, ' ms'),
        statsStageCaptureEncode: formatValue(jitter.avgCaptureEncodeMs, 1, ' ms'),
        statsStageEncodeSend: formatValue(jitter.avgEncodeSendMs, 1, ' ms'),
        statsStageSendReceive: formatValue(jitter.avgSendReceiveMs, 1, ' ms'),
        statsStageReceiveAssemble: formatValue(jitter.avgReceiveAssembleMs, 1, ' ms'),
        statsStageAssembleDecode: formatValue(jitter.avgAssembleDecodeMs, 1, ' ms'),
        statsStageDecodePresent: formatValue(jitter.avgDecodePresentMs, 1, ' ms'),
        statsInterval: formatValue(jitter.intervalMean, 1, ' ms'),
        statsStdDev: formatValue(jitter.intervalStdDev, 2, ' ms'),
        statsDecodeTime: formatValue(decode.avgDecodeTimeMs, 2, ' ms'),
        statsDecodeQueue: decode.maxQueueSize.toString(),
        statsHwAccel: S.hwAccel ? `Decoder ${S.hwAccel}` : '--',
        statsRenderTime: formatValue(render.avgRenderTimeMs, 2, ' ms'),
        statsRenderFrames: render.renderCount.toString(),
        statsPackets: `${network.packetsReceived} (V:${network.videoPackets} A:${network.audioPackets})`,
        statsPacketSize: network.avgPacketSize > 0 ? `${network.avgPacketSize.toFixed(0)} B` : '-- B',
        statsAudioPackets: audio.packetsReceived.toString(),
        statsAudioDecoded: audio.packetsDecoded.toString(),
        statsAudioBuffer: formatValue(audio.avgBufferHealth, 1, ' ms'),
        statsInputMouse: `${stats.moves} moves, ${stats.clicks} clicks`,
        statsInputKeys: stats.keys.toString(),
        statsUptime: formatUptime(data.uptime),
        statsTotalFrames: session.totalFrames.toLocaleString(),
        statsTotalData: `${(session.totalBytes / 1048576).toFixed(2)} MB`,
        statsHostVersion: S.hostVersion || '--'
    };

    for (const [id, value] of Object.entries(updates)) {
        const el = $(id);
        if (el) el.textContent = value;
    }
    const rttEl = $('statsRtt');
    if (rttEl) {
        const rttClass = clock.rttMs > 100 ? 'error'
            : clock.rttMs > 50 ? 'warn'
            : clock.rttMs > 0 ? 'highlight' : '';
        rttEl.parentElement.className = 'stats-row' + (rttClass ? ` ${rttClass}` : '');
    }
    const totalAudioDrops = audio.packetsDropped + audio.bufferUnderruns + audio.bufferOverflows;
    updateDropSection('statsAudioDropsSection', totalAudioDrops > 0, {
        statsAudioDropped: audio.packetsDropped,
        statsAudioUnderruns: audio.bufferUnderruns,
        statsAudioOverflows: audio.bufferOverflows
    });
    const totalDrops = stats.framesDropped + stats.framesTimeout + jitter.framesDroppedLate + stats.decodeErrors;
    updateDropSection('statsDropsSection', totalDrops > 0, {
        statsDropsDropped: stats.framesDropped,
        statsDropsTimeout: stats.framesTimeout,
        statsDropsLate: jitter.framesDroppedLate,
        statsDecodeErrors: stats.decodeErrors
    });
};

const updateStatsVisibility = () => {
    statsOverlay.classList.toggle('visible', statsEnabled);
    statsToggle.classList.toggle('on', statsEnabled);
    const shouldSubscribe = statsEnabled && !metricsUnsubscribe;
    const shouldUnsubscribe = !statsEnabled && metricsUnsubscribe;
    if (shouldSubscribe) {
        metricsUnsubscribe = subscribeToMetrics(updateStats);
    } else if (shouldUnsubscribe) {
        metricsUnsubscribe();
        metricsUnsubscribe = null;
    }
    if (shouldSubscribe || shouldUnsubscribe) log.info('UI', `Stats overlay ${statsEnabled ? 'enabled' : 'disabled'}`);
};

statsToggle.onclick = () => {
    statsEnabled = !statsEnabled;
    savePref(STORAGE_KEYS.STATS, statsEnabled);
    updateStatsVisibility();
};

updateStatsVisibility();
const createToggle = (btnId, storageKey, onChange) => {
    const btn = $(btnId);
    if (!btn) return { get: () => false, set: () => {} };

    let enabled = storageKey ? loadBoolPref(storageKey) : false;

    const update = () => {
        btn.classList.toggle('on', enabled);
        onChange?.(enabled);
    };

    btn.onclick = () => {
        enabled = !enabled;
        if (storageKey) savePref(storageKey, enabled);
        update();
        log.debug('UI', 'Toggle changed', { id: btnId, enabled });
    };

    update();
    return { get: () => enabled, set: v => { enabled = v; update(); } };
};
const relativeMouse = createToggle('relativeMouseBtn', null, setRelativeMouseMode);

window.addEventListener('pointerlockchange', e => {
    if (e.detail?.relativeMouseDisabled) {
        relativeMouse.set(false);
    }
});
S.clipboardSyncEnabled = loadBoolPref(STORAGE_KEYS.CLIPBOARD);
createToggle('clipboardSyncBtn', STORAGE_KEYS.CLIPBOARD, enabled => {
    S.clipboardSyncEnabled = enabled;
});
