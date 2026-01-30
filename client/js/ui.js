import { S, $, Stage, CODECS, detectBestCodec, getCodecSupport, subscribeToMetrics } from './state.js';
import { toggleAudio } from './media.js';
import { setRelativeMouseMode } from './input.js';

const loadingEl = $('loadingOverlay'), statusEl = $('loadingStatus'), subEl = $('loadingSubstatus');

const stageMessages = {
    [Stage.IDLE]: ['Connecting...', 'Initializing'], [Stage.CONNECT]: ['Connecting...', 'Establishing connection'],
    [Stage.AUTH]: ['Authenticating...', 'Verifying credentials'], [Stage.OK]: ['Connected', ''], [Stage.ERR]: ['Failed', 'Retrying...']
};

let keyboardLocked = false;

export const updateLoadingStage = (stage, errorMsg = null) => {
    S.stage = stage;
    const [status, substatus] = stageMessages[stage] || stageMessages[Stage.IDLE];
    statusEl.textContent = S.isReconnecting ? 'Reconnecting...' : status;
    subEl.textContent = errorMsg || substatus;
    loadingEl.classList.toggle('reconnecting', S.isReconnecting);
};

export const showLoading = (isReconnect = false) => {
    S.isReconnecting = isReconnect;
    S.firstFrameReceived = false;
    loadingEl.classList.remove('hidden');
    updateLoadingStage(Stage.IDLE);
};

export const hideLoading = () => {
    S.firstFrameReceived = true;
    updateLoadingStage(Stage.OK);
    setTimeout(() => loadingEl.classList.add('hidden'), 300);
};

let applyFpsFn = null, sendMonFn = null, applyCodecFn = null;
export const setNetCbs = (fpsCb, monCb, codecCb) => { applyFpsFn = fpsCb; sendMonFn = monCb; applyCodecFn = codecCb; };

const panel = $('pnl'), fpsSel = $('fpsSel'), monSel = $('monSel'), codecSel = $('codecSel');
const customFpsRow = $('customFpsRow'), customFpsInput = $('customFpsInput'), customFpsApply = $('customFpsApply');
const togglePanel = on => ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on));

$('edge').onclick = () => togglePanel(true);
$('pnlX').onclick = $('bk').onclick = () => togglePanel(false);
document.onkeydown = e => { if (e.key === 'Escape' && panel.classList.contains('on') && !S.controlEnabled) togglePanel(false); };

const FPS_PREF_KEY = 'slipstream_fps';
const loadFpsPreference = () => { try { const s = localStorage.getItem(FPS_PREF_KEY); if (s !== null) { const f = parseInt(s, 10); if (f >= 1 && f <= 240) return f; } } catch {} return null; };
const saveFpsPreference = fps => { try { localStorage.setItem(FPS_PREF_KEY, fps.toString()); } catch {} };
export const getStoredFps = () => loadFpsPreference();

export const updateFpsDropdown = fps => {
    const fpsNum = +fps, standardOptions = ['15', '30', '60', '120', '144'];
    if (standardOptions.includes(fpsNum.toString())) {
        fpsSel.value = fpsNum.toString();
        customFpsRow.style.display = 'none';
    } else {
        fpsSel.value = 'custom';
        customFpsInput.value = fpsNum;
        customFpsRow.style.display = 'flex';
    }
};

fpsSel.onchange = () => {
    if (fpsSel.value === 'custom') {
        customFpsRow.style.display = 'flex';
    } else {
        customFpsRow.style.display = 'none';
        const fps = +fpsSel.value;
        saveFpsPreference(fps);
        applyFpsFn?.(fps);
    }
};

customFpsApply.onclick = () => {
    const val = parseInt(customFpsInput.value, 10);
    if (val >= 1 && val <= 240) { saveFpsPreference(val); applyFpsFn?.(val); }
};

customFpsInput.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); customFpsApply.click(); } };

monSel.onchange = () => sendMonFn?.(+monSel.value);
codecSel.onchange = () => { const codecId = +codecSel.value; saveCodecPreference(codecId); applyCodecFn?.(codecId); };
$('aBtn').onclick = toggleAudio;

const fsIcon = $('fsi'), fsPath = $('fsp'), fsText = $('fst');
const FS_PATHS = [
    'M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3',
    'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'
];

fsIcon.setAttribute('fill', 'none');
fsIcon.setAttribute('stroke', 'currentColor');
fsIcon.setAttribute('stroke-width', '2');

const updateFullscreenUI = () => {
    const fs = !!document.fullscreenElement;
    fsPath.setAttribute('d', FS_PATHS[fs ? 1 : 0]);
    fsText.textContent = fs ? 'Exit Fullscreen' : 'Fullscreen';
};
updateFullscreenUI();

$('fs').onclick = () => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen?.().catch(() => {});

const supportsKeyboardLock = 'keyboard' in navigator && 'lock' in navigator.keyboard;
document.addEventListener('fullscreenchange', async () => {
    updateFullscreenUI();
    if (!supportsKeyboardLock) return;
    if (document.fullscreenElement) { try { await navigator.keyboard.lock(['Escape']); keyboardLocked = true; } catch { keyboardLocked = false; } }
    else { navigator.keyboard.unlock(); keyboardLocked = false; }
});

const CODEC_PREF_KEY = 'slipstream_codec';
let detectedDefaultCodec = null;

const loadCodecPreference = () => { try { const s = localStorage.getItem(CODEC_PREF_KEY); if (s !== null) { const c = parseInt(s, 10); if (c === 0 || c === 1) return c; } } catch {} return null; };
const saveCodecPreference = codecId => { try { localStorage.setItem(CODEC_PREF_KEY, codecId.toString()); } catch {} };

export const getStoredCodec = () => loadCodecPreference() ?? detectedDefaultCodec ?? 1;

export const initCodecDetection = async () => {
    try {
        const result = await detectBestCodec();
        detectedDefaultCodec = result.codecId;
        console.log(`[CODEC] Default codec set to: ${result.codecName} (${result.hardwareAccel ? 'HW' : 'SW'})`);
        return result;
    } catch {
        detectedDefaultCodec = 1;
        return { codecId: 1, hardwareAccel: false, codecName: 'AV1' };
    }
};

export const updateCodecOpts = async () => {
    const support = await getCodecSupport();
    codecSel.innerHTML = Object.entries(CODECS).map(([key, c]) => {
        const k = key.toLowerCase(), supported = support[k], hw = support[`${k}Hw`];
        const suffix = !supported ? ' (unsupported)' : hw ? ' (HW)' : ' (SW)';
        return `<option value="${c.id}" ${!supported ? 'disabled' : ''}>${c.name}${suffix}</option>`;
    }).join('');

    const stored = loadCodecPreference(), codecToSelect = stored ?? detectedDefaultCodec ?? 1;
    const selectedSupported = codecToSelect === 1 ? support.av1 : support.h264;
    const finalCodec = selectedSupported ? codecToSelect : (support.av1 ? 1 : 0);
    codecSel.value = finalCodec;
    S.currentCodec = finalCodec;
};

const tabStrip = $('tabStrip'), tabContainer = $('tabContainer');
const tabbedModeBtn = $('tabbedModeBtn'), tabBackBtn = $('tabBack');
const TABBED_MODE_KEY = 'slipstream_tabbed_mode';

const monitorIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="2" ry="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>`;

const loadTabbedMode = () => { try { return localStorage.getItem(TABBED_MODE_KEY) === 'true'; } catch { return false; } };
const saveTabbedMode = on => { try { localStorage.setItem(TABBED_MODE_KEY, on ? 'true' : 'false'); } catch {} };

const renderTabs = () => {
    if (!S.monitors.length) { tabContainer.innerHTML = ''; return; }
    tabContainer.innerHTML = S.monitors.map(m => `<button class="tab-item${m.index === S.currentMon ? ' active' : ''}" data-index="${m.index}">${monitorIcon}<div class="tab-item-info"><span class="tab-item-name">${m.name || `Display ${m.index + 1}`}${m.isPrimary ? '*' : ''}</span><span class="tab-item-res">(${m.width}x${m.height})</span></div></button>`).join('');
    tabContainer.querySelectorAll('.tab-item').forEach(t => { t.onclick = () => { const i = +t.dataset.index; if (i !== S.currentMon && sendMonFn) sendMonFn(i); }; });
};

const triggerCanvasResize = () => {
    // Dispatch resize event after CSS transition completes to ensure canvas updates
    setTimeout(() => window.dispatchEvent(new Event('resize')), 350);
};

const updateTabbedModeUI = () => {
    tabbedModeBtn.classList.toggle('on', S.tabbedMode);
    document.body.classList.toggle('tabbed-mode', S.tabbedMode);
    const show = S.tabbedMode && S.monitors.length > 0;
    tabStrip.classList.toggle('visible', show);
    if (show) renderTabs();
    triggerCanvasResize();
};

S.tabbedMode = loadTabbedMode();
tabbedModeBtn.onclick = () => { S.tabbedMode = !S.tabbedMode; saveTabbedMode(S.tabbedMode); updateTabbedModeUI(); };
tabBackBtn.onclick = () => { S.tabbedMode = false; saveTabbedMode(false); updateTabbedModeUI(); };

// Apply tabbed mode on load without triggering resize (will be handled by initial render)
if (S.tabbedMode) {
    tabbedModeBtn.classList.add('on');
    document.body.classList.add('tabbed-mode');
    if (S.monitors.length > 0) {
        tabStrip.classList.add('visible');
        renderTabs();
    }
}

export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length ? S.monitors.map(m => `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`).join('') : '<option value="0">Waiting...</option>';
    monSel.value = S.currentMon;
    // Update tabs if in tabbed mode, but don't trigger resize (just update content)
    if (S.tabbedMode) {
        const show = S.monitors.length > 0;
        tabStrip.classList.toggle('visible', show);
        if (show) renderTabs();
    }
};

const statsOverlay = $('statsOverlay'), statsToggleBtn = $('statsToggleBtn');
const STATS_OVERLAY_KEY = 'slipstream_stats_overlay';

let statsEnabled = false, unsubscribeMetrics = null;

const loadStatsPreference = () => { try { return localStorage.getItem(STATS_OVERLAY_KEY) === 'true'; } catch { return false; } };
const saveStatsPreference = on => { try { localStorage.setItem(STATS_OVERLAY_KEY, on ? 'true' : 'false'); } catch {} };

const updateStatsOverlayVisibility = () => {
    statsOverlay.classList.toggle('visible', statsEnabled);
    statsToggleBtn.classList.toggle('on', statsEnabled);
    if (statsEnabled && !unsubscribeMetrics) unsubscribeMetrics = subscribeToMetrics(updateStatsDisplay);
    else if (!statsEnabled && unsubscribeMetrics) { unsubscribeMetrics(); unsubscribeMetrics = null; }
};

const updateStatsDisplay = data => {
    if (!statsEnabled) return;
    const { stats, jitter, clock, network, decode, render, session, computed } = data;

    $('statsFps').textContent = `${computed.fps}/${computed.targetFps} (${computed.fpsEff.toFixed(0)}%)`;
    $('statsBitrate').textContent = `${computed.mbps} Mbps`;
    $('statsResolution').textContent = S.W > 0 ? `${S.W}x${S.H}` : '--x--';
    $('statsCodec').textContent = S.currentCodec === 1 ? `AV1 (${S.hwAccel})` : `H.264 (${S.hwAccel})`;

    $('statsRtt').textContent = clock.valid ? `${clock.rttMs.toFixed(1)} ms` : '-- ms';
    $('statsFrameAge').textContent = jitter.avgServerAgeMs > 0 ? `${jitter.avgServerAgeMs.toFixed(1)} ms` : '-- ms';

    const rttRow = $('statsRtt').parentElement;
    rttRow.className = 'stats-row' + (clock.rttMs > 100 ? ' error' : clock.rttMs > 50 ? ' warn' : clock.rttMs > 0 ? ' highlight' : '');

    $('statsQueue').textContent = `${jitter.avgQueueDepth.toFixed(1)} (max:${jitter.maxQueueDepth})`;
    $('statsInterval').textContent = jitter.intervalMean > 0 ? `${jitter.intervalMean.toFixed(1)} ms` : '-- ms';
    $('statsStdDev').textContent = jitter.intervalStdDev > 0 ? `${jitter.intervalStdDev.toFixed(2)} ms` : '-- ms';

    $('statsDecodeTime').textContent = decode.decodeCount > 0 ? `${decode.avgDecodeTimeMs.toFixed(2)} ms` : '-- ms';
    $('statsDecodeQueue').textContent = decode.maxQueueSize.toString();
    $('statsHwAccel').textContent = S.hwAccel || '--';

    $('statsRenderTime').textContent = render.renderCount > 0 ? `${render.avgRenderTimeMs.toFixed(2)} ms` : '-- ms';
    $('statsRenderFrames').textContent = render.renderCount.toString();

    $('statsPackets').textContent = `${network.packetsReceived} (V:${network.videoPackets} A:${network.audioPackets})`;
    $('statsPacketSize').textContent = network.avgPacketSize > 0 ? `${network.avgPacketSize.toFixed(0)} B` : '-- B';

    const totalDrops = stats.framesDropped + stats.framesTimeout + jitter.framesDroppedLate + jitter.framesDroppedOverflow + stats.decodeErrors;
    const dropsSection = $('statsDropsSection');
    dropsSection.classList.toggle('has-drops', totalDrops > 0);

    if (totalDrops > 0) {
        $('statsDropsLate').textContent = jitter.framesDroppedLate.toString();
        $('statsDropsOverflow').textContent = jitter.framesDroppedOverflow.toString();
        $('statsDecodeErrors').textContent = stats.decodeErrors.toString();
    }

    $('statsInputMouse').textContent = `${stats.moves} moves, ${stats.clicks} clicks`;
    $('statsInputKeys').textContent = stats.keys.toString();

    $('statsUptime').textContent = `${data.uptime}s`;
    $('statsTotalFrames').textContent = session.totalFrames.toLocaleString();
    $('statsTotalData').textContent = `${(session.totalBytes / 1048576).toFixed(2)} MB`;
};

statsEnabled = loadStatsPreference();
statsToggleBtn.onclick = () => { statsEnabled = !statsEnabled; saveStatsPreference(statsEnabled); updateStatsOverlayVisibility(); };
updateStatsOverlayVisibility();

const relativeMouseBtn = $('relativeMouseBtn'), relativeMouseHint = $('relativeMouseHint');
let relativeMouseEnabled = false;

const updateRelativeMouseUI = () => {
    relativeMouseBtn.classList.toggle('on', relativeMouseEnabled);
    relativeMouseHint.classList.toggle('visible', relativeMouseEnabled);
    setRelativeMouseMode(relativeMouseEnabled);
};

relativeMouseBtn.onclick = () => { relativeMouseEnabled = !relativeMouseEnabled; updateRelativeMouseUI(); };
updateRelativeMouseUI();

window.addEventListener('pointerlockchange', e => {
    if (e.detail?.relativeMouseDisabled) { relativeMouseEnabled = false; updateRelativeMouseUI(); }
});
