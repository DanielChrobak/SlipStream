import { S, $, Stage } from './state.js';
import { toggleAudio } from './media.js';

const loadingEl = $('loadingOverlay');
const statusEl = $('loadingStatus');
const subEl = $('loadingSubstatus');

const stageMessages = {
    [Stage.IDLE]: ['Connecting...', 'Initializing'],
    [Stage.CONNECT]: ['Connecting...', 'Establishing connection'],
    [Stage.AUTH]: ['Authenticating...', 'Verifying credentials'],
    [Stage.OK]: ['Connected', ''],
    [Stage.ERR]: ['Failed', 'Retrying...']
};

const supportsKeyboardLock = 'keyboard' in navigator && 'lock' in navigator.keyboard;
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

export const isLoadingVisible = () => !loadingEl.classList.contains('hidden');

let applyFpsFn = null;
let sendMonFn = null;

export const setNetCbs = (fpsCallback, monitorCallback) => {
    applyFpsFn = fpsCallback;
    sendMonFn = monitorCallback;
};

const panel = $('pnl');
const fpsSel = $('fpsSel');
const monSel = $('monSel');

const togglePanel = on => { ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on)); };

$('edge').onclick = () => togglePanel(true);
$('pnlX').onclick = () => togglePanel(false);
$('bk').onclick = () => togglePanel(false);

document.onkeydown = e => {
    if (e.key === 'Escape' && panel.classList.contains('on') && !S.controlEnabled) togglePanel(false);
};

fpsSel.onchange = () => applyFpsFn?.(fpsSel.value);
monSel.onchange = () => sendMonFn?.(+monSel.value);
$('aBtn').onclick = toggleAudio;

const fsIcon = $('fsi');
const fsPath = $('fsp');
const fsText = $('fst');

const FS_PATHS = [
    'M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3',
    'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'
];

const isFullscreen = () => !!document.fullscreenElement;

fsIcon.setAttribute('fill', 'none');
fsIcon.setAttribute('stroke', 'currentColor');
fsIcon.setAttribute('stroke-width', '2');

const updateFullscreenUI = () => {
    const isFs = isFullscreen();
    fsPath.setAttribute('d', FS_PATHS[isFs ? 1 : 0]);
    fsText.textContent = isFs ? 'Exit Fullscreen' : 'Fullscreen';
};

updateFullscreenUI();

$('fs').onclick = () => {
    if (isFullscreen()) document.exitFullscreen();
    else document.documentElement.requestFullscreen?.().catch(e => console.warn('Fullscreen:', e.message));
};

document.addEventListener('fullscreenchange', async () => {
    updateFullscreenUI();
    if (supportsKeyboardLock) {
        if (document.fullscreenElement) {
            try { await navigator.keyboard.lock(['Escape']); keyboardLocked = true; }
            catch { keyboardLocked = false; }
        } else {
            navigator.keyboard.unlock();
            keyboardLocked = false;
        }
    }
});

export const exitFullscreen = () => { if (isFullscreen()) document.exitFullscreen(); };

const tabStrip = $('tabStrip');
const tabContainer = $('tabContainer');
const tabbedModeBtn = $('tabbedModeBtn');
const tabBackBtn = $('tabBack');
const TABBED_MODE_KEY = 'slipstream_tabbed_mode';

const monitorIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
    <rect x="2" y="3" width="20" height="14" rx="2" ry="2"/>
    <line x1="8" y1="21" x2="16" y2="21"/>
    <line x1="12" y1="17" x2="12" y2="21"/>
</svg>`;

const loadTabbedMode = () => {
    try { return localStorage.getItem(TABBED_MODE_KEY) === 'true'; }
    catch { return false; }
};

const saveTabbedMode = enabled => {
    try { localStorage.setItem(TABBED_MODE_KEY, enabled ? 'true' : 'false'); } catch {}
};

const updateTabbedModeUI = () => {
    const enabled = S.tabbedMode;
    tabbedModeBtn.classList.toggle('on', enabled);
    document.body.classList.toggle('tabbed-mode', enabled);

    const shouldShow = enabled && S.monitors.length > 0;
    tabStrip.classList.toggle('visible', shouldShow);
    if (shouldShow) renderTabs();
};

const renderTabs = () => {
    if (!S.monitors.length) { tabContainer.innerHTML = ''; return; }

    tabContainer.innerHTML = S.monitors.map(monitor => {
        const displayName = monitor.name || `Display ${monitor.index + 1}`;
        const resolution = `${monitor.width}x${monitor.height}`;
        const isActive = monitor.index === S.currentMon;
        return `<button class="tab-item${isActive ? ' active' : ''}" data-index="${monitor.index}">
            ${monitorIcon}
            <div class="tab-item-info">
                <span class="tab-item-name">${displayName}${monitor.isPrimary ? '*' : ''}</span>
                <span class="tab-item-res">(${resolution})</span>
            </div>
        </button>`;
    }).join('');

    tabContainer.querySelectorAll('.tab-item').forEach(tab => {
        tab.onclick = () => {
            const index = parseInt(tab.dataset.index, 10);
            if (index !== S.currentMon && sendMonFn) sendMonFn(index);
        };
    });
};

const toggleTabbedMode = () => {
    S.tabbedMode = !S.tabbedMode;
    saveTabbedMode(S.tabbedMode);
    updateTabbedModeUI();
};

S.tabbedMode = loadTabbedMode();
tabbedModeBtn.onclick = toggleTabbedMode;
tabBackBtn.onclick = () => { S.tabbedMode = false; saveTabbedMode(false); updateTabbedModeUI(); };
updateTabbedModeUI();

export const updateMonOpts = () => {
    if (S.monitors.length) {
        monSel.innerHTML = S.monitors.map(m => {
            const label = `#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}`;
            return `<option value="${m.index}">${label}</option>`;
        }).join('');
    } else {
        monSel.innerHTML = '<option value="0">Waiting...</option>';
    }
    monSel.value = S.currentMon;
    if (S.tabbedMode) updateTabbedModeUI();
};

export const updateFpsOpts = () => {
    const prevValue = fpsSel.value;
    const fpsValues = [...new Set([15, 30, 60, S.hostFps, S.clientFps])].sort((a, b) => a - b);

    fpsSel.innerHTML = fpsValues.map(fps => {
        let label = String(fps);
        if (fps === S.hostFps && fps === S.clientFps) label += ' (Native)';
        else if (fps === S.hostFps) label += ' (Host)';
        else if (fps === S.clientFps) label += ' (Client)';
        return `<option value="${fps}">${label}</option>`;
    }).join('');

    if ([...fpsSel.options].some(o => o.value === prevValue)) fpsSel.value = prevValue;
};
