import { S, $, CODECS, detectCodecs, subscribeToMetrics } from './state.js';
import { toggleAudio } from './media.js';
import { setRelativeMouseMode } from './input.js';

const loadingEl = $('loadingOverlay'), statusEl = $('loadingStatus'), subEl = $('loadingSubstatus');

export const updateLoadingStage = (status, substatus = '') => {
    statusEl.textContent = S.isReconnecting ? 'Reconnecting...' : status;
    subEl.textContent = substatus;
    loadingEl.classList.toggle('reconnecting', S.isReconnecting);
};

export const showLoading = (isReconnect = false) => {
    S.isReconnecting = isReconnect; S.firstFrameReceived = false;
    loadingEl.classList.remove('hidden'); updateLoadingStage('Connecting...', 'Initializing');
};

export const hideLoading = () => {
    S.firstFrameReceived = true; updateLoadingStage('Connected');
    setTimeout(() => loadingEl.classList.add('hidden'), 300);
};

let applyFpsFn = null, sendMonFn = null, applyCodecFn = null;
export const setNetCbs = (fpsCb, monCb, codecCb) => { applyFpsFn = fpsCb; sendMonFn = monCb; applyCodecFn = codecCb; };

const fpsSel = $('fpsSel'), monSel = $('monSel'), codecSel = $('codecSel');
const customFpsRow = $('customFpsRow'), customFpsInput = $('customFpsInput'), customFpsApply = $('customFpsApply');
const togglePanel = on => ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on));

$('edge').onclick = () => togglePanel(true);
$('pnlX').onclick = $('bk').onclick = () => togglePanel(false);
document.onkeydown = e => { if (e.key === 'Escape' && $('pnl').classList.contains('on') && !S.controlEnabled) togglePanel(false); };

const FPS_KEY = 'slipstream_fps', CODEC_KEY = 'slipstream_codec', TABBED_KEY = 'slipstream_tabbed_mode', STATS_KEY = 'slipstream_stats_overlay';
const loadPref = (key, parse = parseInt, validate = () => true, def = null) => { try { const v = parse(localStorage.getItem(key)); return validate(v) ? v : def; } catch (e) { console.warn(`[UI] Failed to load preference '${key}':`, e.message); return def; } };
const savePref = (key, val) => { try { localStorage.setItem(key, val.toString()); } catch (e) { console.warn(`[UI] Failed to save preference '${key}':`, e.message); } };

export const getStoredFps = () => loadPref(FPS_KEY, parseInt, v => v >= 1 && v <= 240, null);
export const getStoredCodec = () => loadPref(CODEC_KEY, parseInt, v => v === 0 || v === 1, null) ?? detectedDefaultCodec ?? 1;

export const updateFpsDropdown = fps => {
    const fpsNum = +fps, standardOpts = ['15', '30', '60', '120', '144'];
    if (standardOpts.includes(fpsNum.toString())) { fpsSel.value = fpsNum.toString(); customFpsRow.style.display = 'none'; }
    else { fpsSel.value = 'custom'; customFpsInput.value = fpsNum; customFpsRow.style.display = 'flex'; }
};

fpsSel.onchange = () => {
    if (fpsSel.value === 'custom') customFpsRow.style.display = 'flex';
    else { customFpsRow.style.display = 'none'; const fps = +fpsSel.value; savePref(FPS_KEY, fps); applyFpsFn?.(fps); }
};

customFpsApply.onclick = () => { const val = parseInt(customFpsInput.value, 10); if (val >= 1 && val <= 240) { savePref(FPS_KEY, val); applyFpsFn?.(val); } };
customFpsInput.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); customFpsApply.click(); } };

monSel.onchange = () => sendMonFn?.(+monSel.value);
codecSel.onchange = () => { const codecId = +codecSel.value; savePref(CODEC_KEY, codecId); applyCodecFn?.(codecId); };
$('aBtn').onclick = toggleAudio;

const fsPath = $('fsp'), fsText = $('fst');
const FS_PATHS = ['M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3', 'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'];
const fsi = $('fsi'); fsi.setAttribute('fill', 'none'); fsi.setAttribute('stroke', 'currentColor'); fsi.setAttribute('stroke-width', '2');

const updateFullscreenUI = () => {
    const fs = !!document.fullscreenElement;
    fsPath.setAttribute('d', FS_PATHS[fs ? 1 : 0]);
    fsText.textContent = fs ? 'Exit Fullscreen' : 'Fullscreen';
};
updateFullscreenUI();

$('fs').onclick = () => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen?.().catch(e => { console.warn('[UI] Fullscreen request failed:', e.message); });

const supportsKeyboardLock = 'keyboard' in navigator && 'lock' in navigator.keyboard;
document.addEventListener('fullscreenchange', async () => {
    updateFullscreenUI();
    if (!supportsKeyboardLock) return;
    if (document.fullscreenElement) try { await navigator.keyboard.lock(['Escape']); } catch (e) { console.warn('[UI] Keyboard lock failed:', e.message); } else navigator.keyboard.unlock();
});

let detectedDefaultCodec = null;

export const initCodecDetection = async () => {
    try { const { best } = await detectCodecs(); detectedDefaultCodec = best.codecId; return best; }
    catch (e) { console.warn('[UI] Codec detection failed:', e.message); detectedDefaultCodec = 1; return { codecId: 1, hardwareAccel: false, codecName: 'AV1' }; }
};

export const updateCodecOpts = async () => {
    const { support } = await detectCodecs();
    codecSel.innerHTML = Object.entries(CODECS).map(([key, c]) => {
        const k = key.toLowerCase(), supported = support[k], hw = support[`${k}Hw`];
        const suffix = !supported ? ' (unsupported)' : hw ? ' (HW)' : ' (SW)';
        return `<option value="${c.id}" ${!supported ? 'disabled' : ''}>${c.name}${suffix}</option>`;
    }).join('');
    const stored = loadPref(CODEC_KEY, parseInt, v => v === 0 || v === 1, null);
    const codecToSelect = stored ?? detectedDefaultCodec ?? 1;
    const selectedSupported = codecToSelect === 1 ? support.av1 : support.h264;
    const finalCodec = selectedSupported ? codecToSelect : (support.av1 ? 1 : 0);
    codecSel.value = finalCodec; S.currentCodec = finalCodec;
};

const tabStrip = $('tabStrip'), tabContainer = $('tabContainer');
const tabbedModeBtn = $('tabbedModeBtn'), tabBackBtn = $('tabBack');
const monitorIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="2" ry="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>`;
const primaryStarIcon = `<svg class="primary-star" viewBox="0 0 24 24" fill="currentColor" stroke="none"><path d="M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z"/></svg>`;

const renderTabs = () => {
    if (!S.monitors.length) { tabContainer.innerHTML = ''; return; }
    tabContainer.innerHTML = S.monitors.map(m => `<button class="tab-item${m.index === S.currentMon ? ' active' : ''}" data-index="${m.index}">${monitorIcon}<div class="tab-item-info"><span class="tab-item-name">${m.name || `Display ${m.index + 1}`}${m.isPrimary ? primaryStarIcon : ''}</span><span class="tab-item-res">(${m.width}x${m.height})</span></div></button>`).join('');
    tabContainer.querySelectorAll('.tab-item').forEach(t => { t.onclick = () => { const i = +t.dataset.index; if (i !== S.currentMon && sendMonFn) sendMonFn(i); }; });
};

const triggerCanvasResize = () => setTimeout(() => window.dispatchEvent(new Event('resize')), 350);

const updateTabbedModeUI = () => {
    tabbedModeBtn.classList.toggle('on', S.tabbedMode);
    document.body.classList.toggle('tabbed-mode', S.tabbedMode);
    const show = S.tabbedMode && S.monitors.length > 0;
    tabStrip.classList.toggle('visible', show);
    if (show) renderTabs();
    triggerCanvasResize();
};

S.tabbedMode = localStorage.getItem(TABBED_KEY) === 'true';
tabbedModeBtn.onclick = () => { S.tabbedMode = !S.tabbedMode; savePref(TABBED_KEY, S.tabbedMode); updateTabbedModeUI(); };
tabBackBtn.onclick = () => { S.tabbedMode = false; savePref(TABBED_KEY, false); updateTabbedModeUI(); };

if (S.tabbedMode) {
    tabbedModeBtn.classList.add('on'); document.body.classList.add('tabbed-mode');
    if (S.monitors.length > 0) { tabStrip.classList.add('visible'); renderTabs(); }
}

export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length ? S.monitors.map(m => `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`).join('') : '<option value="0">Waiting...</option>';
    monSel.value = S.currentMon;
    if (S.tabbedMode) { const show = S.monitors.length > 0; tabStrip.classList.toggle('visible', show); if (show) renderTabs(); }
};

const statsOverlay = $('statsOverlay'), statsToggleBtn = $('statsToggleBtn');
let statsEnabled = localStorage.getItem(STATS_KEY) === 'true', unsubscribeMetrics = null;

const updateStatsOverlayVisibility = () => {
    statsOverlay.classList.toggle('visible', statsEnabled);
    statsToggleBtn.classList.toggle('on', statsEnabled);
    if (statsEnabled && !unsubscribeMetrics) unsubscribeMetrics = subscribeToMetrics(updateStatsDisplay);
    else if (!statsEnabled && unsubscribeMetrics) { unsubscribeMetrics(); unsubscribeMetrics = null; }
};

const updateStatsDisplay = data => {
    if (!statsEnabled) return;
    const { stats, jitter, clock, network, decode, render, audio, session, computed } = data;

    $('statsFps').textContent = `${computed.fps}/${computed.targetFps} (${computed.fpsEff.toFixed(0)}%)`;
    $('statsBitrate').textContent = `${computed.mbps} Mbps`;
    $('statsResolution').textContent = S.W > 0 ? `${S.W}x${S.H}` : '--x--';
    $('statsCodec').textContent = S.currentCodec === 1 ? `AV1 (${S.hwAccel})` : `H.264 (${S.hwAccel})`;
    $('statsRtt').textContent = clock.valid ? `${clock.rttMs.toFixed(1)} ms` : '-- ms';
    $('statsFrameAge').textContent = jitter.avgServerAgeMs > 0 ? `${jitter.avgServerAgeMs.toFixed(1)} ms` : '-- ms';
    $('statsRtt').parentElement.className = 'stats-row' + (clock.rttMs > 100 ? ' error' : clock.rttMs > 50 ? ' warn' : clock.rttMs > 0 ? ' highlight' : '');

    $('statsInterval').textContent = jitter.intervalMean > 0 ? `${jitter.intervalMean.toFixed(1)} ms` : '-- ms';
    $('statsStdDev').textContent = jitter.intervalStdDev > 0 ? `${jitter.intervalStdDev.toFixed(2)} ms` : '-- ms';
    $('statsDecodeTime').textContent = decode.decodeCount > 0 ? `${decode.avgDecodeTimeMs.toFixed(2)} ms` : '-- ms';
    $('statsDecodeQueue').textContent = decode.maxQueueSize.toString();
    $('statsHwAccel').textContent = S.hwAccel || '--';
    $('statsRenderTime').textContent = render.renderCount > 0 ? `${render.avgRenderTimeMs.toFixed(2)} ms` : '-- ms';
    $('statsRenderFrames').textContent = render.renderCount.toString();
    $('statsPackets').textContent = `${network.packetsReceived} (V:${network.videoPackets} A:${network.audioPackets})`;
    $('statsPacketSize').textContent = network.avgPacketSize > 0 ? `${network.avgPacketSize.toFixed(0)} B` : '-- B';

    $('statsAudioPackets').textContent = audio.packetsReceived.toString();
    $('statsAudioDecoded').textContent = audio.packetsDecoded.toString();
    $('statsAudioBuffer').textContent = audio.avgBufferHealth > 0 ? `${audio.avgBufferHealth.toFixed(1)} ms` : '-- ms';

    const totalAudioDrops = audio.packetsDropped + audio.bufferUnderruns + audio.bufferOverflows;
    const audioDropsSection = $('statsAudioDropsSection');
    if (audioDropsSection) {
        audioDropsSection.classList.toggle('has-drops', totalAudioDrops > 0);
        if (totalAudioDrops > 0) {
            $('statsAudioDropped').textContent = audio.packetsDropped.toString();
            $('statsAudioUnderruns').textContent = audio.bufferUnderruns.toString();
            $('statsAudioOverflows').textContent = audio.bufferOverflows.toString();
        }
    }

    const totalDrops = stats.framesDropped + stats.framesTimeout + jitter.framesDroppedLate + stats.decodeErrors;
    const dropsSection = $('statsDropsSection');
    dropsSection.classList.toggle('has-drops', totalDrops > 0);
    if (totalDrops > 0) {
        $('statsDropsDropped').textContent = stats.framesDropped.toString();
        $('statsDropsTimeout').textContent = stats.framesTimeout.toString();
        $('statsDropsLate').textContent = jitter.framesDroppedLate.toString();
        $('statsDecodeErrors').textContent = stats.decodeErrors.toString();
    }

    $('statsInputMouse').textContent = `${stats.moves} moves, ${stats.clicks} clicks`;
    $('statsInputKeys').textContent = stats.keys.toString();
    $('statsUptime').textContent = `${data.uptime}s`;
    $('statsTotalFrames').textContent = session.totalFrames.toLocaleString();
    $('statsTotalData').textContent = `${(session.totalBytes / 1048576).toFixed(2)} MB`;
};

statsToggleBtn.onclick = () => { statsEnabled = !statsEnabled; savePref(STATS_KEY, statsEnabled); updateStatsOverlayVisibility(); };
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

window.addEventListener('pointerlockchange', e => { if (e.detail?.relativeMouseDisabled) { relativeMouseEnabled = false; updateRelativeMouseUI(); } });

// Clipboard sync toggle
const CLIPBOARD_KEY = 'slipstream_clipboard_sync';
const clipboardSyncBtn = $('clipboardSyncBtn');
const clipboardSyncHint = $('clipboardSyncHint');

const updateClipboardSyncUI = () => {
    clipboardSyncBtn.classList.toggle('on', S.clipboardSyncEnabled);
    clipboardSyncHint.classList.toggle('visible', S.clipboardSyncEnabled);
};

S.clipboardSyncEnabled = localStorage.getItem(CLIPBOARD_KEY) === 'true';

clipboardSyncBtn.onclick = () => {
    S.clipboardSyncEnabled = !S.clipboardSyncEnabled;
    savePref(CLIPBOARD_KEY, S.clipboardSyncEnabled);
    updateClipboardSyncUI();
};

updateClipboardSyncUI();
