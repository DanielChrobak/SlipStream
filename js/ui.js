import { S, $, Stage, CODECS, detectBestCodec, getCodecSupport } from './state.js';
import { toggleAudio } from './media.js';

const loadingEl = $('loadingOverlay'), statusEl = $('loadingStatus'), subEl = $('loadingSubstatus');

const stageMessages = {
    [Stage.IDLE]: ['Connecting...', 'Initializing'], [Stage.CONNECT]: ['Connecting...', 'Establishing connection'],
    [Stage.AUTH]: ['Authenticating...', 'Verifying credentials'], [Stage.OK]: ['Connected', ''], [Stage.ERR]: ['Failed', 'Retrying...']
};

let keyboardLocked = false;
export const isKeyboardLocked = () => keyboardLocked;

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
const togglePanel = on => ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on));

$('edge').onclick = () => togglePanel(true);
$('pnlX').onclick = $('bk').onclick = () => togglePanel(false);
document.onkeydown = e => { if (e.key === 'Escape' && panel.classList.contains('on') && !S.controlEnabled) togglePanel(false); };

fpsSel.onchange = () => applyFpsFn?.(fpsSel.value);
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

export const exitFullscreen = () => { if (document.fullscreenElement) document.exitFullscreen(); };

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

const updateTabbedModeUI = () => {
    tabbedModeBtn.classList.toggle('on', S.tabbedMode);
    document.body.classList.toggle('tabbed-mode', S.tabbedMode);
    const show = S.tabbedMode && S.monitors.length > 0;
    tabStrip.classList.toggle('visible', show);
    if (show) renderTabs();
};

S.tabbedMode = loadTabbedMode();
tabbedModeBtn.onclick = () => { S.tabbedMode = !S.tabbedMode; saveTabbedMode(S.tabbedMode); updateTabbedModeUI(); };
tabBackBtn.onclick = () => { S.tabbedMode = false; saveTabbedMode(false); updateTabbedModeUI(); };
updateTabbedModeUI();

export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length ? S.monitors.map(m => `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`).join('') : '<option value="0">Waiting...</option>';
    monSel.value = S.currentMon;
    if (S.tabbedMode) updateTabbedModeUI();
};

export const updateFpsOpts = () => {
    const prev = fpsSel.value;
    const vals = [...new Set([15, 30, 60, S.hostFps, S.clientFps])].sort((a, b) => a - b);
    fpsSel.innerHTML = vals.map(f => {
        const lbl = f === S.hostFps && f === S.clientFps ? ' (Native)' : f === S.hostFps ? ' (Host)' : f === S.clientFps ? ' (Client)' : '';
        return `<option value="${f}">${f}${lbl}</option>`;
    }).join('');
    if ([...fpsSel.options].some(o => o.value === prev)) fpsSel.value = prev;
};
