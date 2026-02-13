import { S, $, CODECS, detectCodecs, subscribeToMetrics, safe } from './state.js';
import { toggleAudio } from './media.js';
import { setRelativeMouseMode } from './input.js';
import { toggleMic, isMicSupported } from './mic.js';

const loadEl = $('loadingOverlay'), statEl = $('loadingStatus'), subEl = $('loadingSubstatus');

export const updateLoadingStage = (st, sub = '') => { statEl.textContent = S.isReconnecting ? 'Reconnecting...' : st; subEl.textContent = sub; loadEl.classList.toggle('reconnecting', S.isReconnecting); };
export const showLoading = (isRec = 0) => { S.isReconnecting = isRec; S.firstFrameReceived = 0; loadEl.classList.remove('hidden'); updateLoadingStage('Connecting...', 'Initializing'); };
export const hideLoading = () => { S.firstFrameReceived = 1; updateLoadingStage('Connected'); setTimeout(() => loadEl.classList.add('hidden'), 300); };

let applyFpsFn = null, sendMonFn = null, applyCodecFn = null;
export const setNetCbs = (fps, mon, codec) => { applyFpsFn = fps; sendMonFn = mon; applyCodecFn = codec; };

const fpsSel = $('fpsSel'), monSel = $('monSel'), codecSel = $('codecSel'), custRow = $('customFpsRow'), custIn = $('customFpsInput'), custApp = $('customFpsApply');
const togPnl = on => ['pnl', 'bk', 'edge'].forEach(id => $(id).classList.toggle('on', on));

$('edge').onclick = () => togPnl(1);
$('pnlX').onclick = $('bk').onclick = () => togPnl(0);
document.onkeydown = e => { if (e.key === 'Escape' && $('pnl').classList.contains('on') && !S.controlEnabled) togPnl(0); };

const K = { FPS: 'slipstream_fps', COD: 'slipstream_codec', TAB: 'slipstream_tabbed_mode', STA: 'slipstream_stats_overlay', CLI: 'slipstream_clipboard_sync' };
const ldPref = (k, val, def = null) => { try { const v = parseInt(localStorage.getItem(k)); return val(v) ? v : def; } catch { return def; } };
const svPref = (k, v) => safe(() => localStorage.setItem(k, v.toString()));

export const getStoredFps = () => ldPref(K.FPS, v => v >= 1 && v <= 240, null);
export const getStoredCodec = () => ldPref(K.COD, v => v >= 0 && v <= 2, null) ?? defCodec ?? 0;

export const updateFpsDropdown = fps => {
    const n = +fps, std = ['15', '30', '60', '120', '144'];
    if (std.includes(n.toString())) { fpsSel.value = n.toString(); custRow.style.display = 'none'; }
    else { fpsSel.value = 'custom'; custIn.value = n; custRow.style.display = 'flex'; }
};

fpsSel.onchange = () => { if (fpsSel.value === 'custom') custRow.style.display = 'flex'; else { custRow.style.display = 'none'; const f = +fpsSel.value; svPref(K.FPS, f); applyFpsFn?.(f); } };
custApp.onclick = () => { const v = parseInt(custIn.value, 10); if (v >= 1 && v <= 240) { svPref(K.FPS, v); applyFpsFn?.(v); } };
custIn.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); custApp.click(); } };

monSel.onchange = () => sendMonFn?.(+monSel.value);
codecSel.onchange = () => { const c = +codecSel.value; svPref(K.COD, c); applyCodecFn?.(c); };
$('aBtn').onclick = toggleAudio;

const micBtn = $('micBtn'), micTxt = $('micTxt'), micHnt = $('micHint');
const updMicUI = () => { if (!micBtn) return; micBtn.classList.toggle('on', S.micEnabled); micTxt.textContent = S.micEnabled ? 'Disable' : 'Enable'; micHnt?.classList.toggle('visible', !isMicSupported()); };
if (micBtn) { micBtn.onclick = async () => { await toggleMic(); updMicUI(); }; if (micHnt && !isMicSupported()) { micHnt.textContent = 'Mic not supported'; micHnt.classList.add('visible'); } }

const fsPath = $('fsp'), fsTxt = $('fst'), FS_P = ['M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3', 'M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3'];
const fsi = $('fsi'); fsi.setAttribute('fill', 'none'); fsi.setAttribute('stroke', 'currentColor'); fsi.setAttribute('stroke-width', '2');
const updFS = () => { const fs = !!document.fullscreenElement; fsPath.setAttribute('d', FS_P[fs ? 1 : 0]); fsTxt.textContent = fs ? 'Exit Fullscreen' : 'Fullscreen'; };
updFS();

$('fs').onclick = () => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen?.().catch(() => {});

const kbLock = 'keyboard' in navigator && 'lock' in navigator.keyboard;
document.addEventListener('fullscreenchange', async () => { updFS(); if (!kbLock) return; if (document.fullscreenElement) safe(() => navigator.keyboard.lock(['Escape'])); else navigator.keyboard.unlock(); });

let defCodec = null;
export const initCodecDetection = async () => { try { const { best } = await detectCodecs(); defCodec = best.codecId; return best; } catch { defCodec = 1; return { codecId: 1, hardwareAccel: 0, codecName: 'H.265' }; } };

export const updateCodecOpts = async () => {
    const { support } = await detectCodecs();
    codecSel.innerHTML = Object.entries(CODECS).map(([k, c]) => { const l = k.toLowerCase(), s = support[l], hw = support[l+'Hw'], suf = !s ? ' (unsupported)' : hw ? ' (HW)' : ' (SW)'; return `<option value="${c.id}" ${!s ? 'disabled' : ''}>${c.name}${suf}</option>`; }).join('');
    const st = ldPref(K.COD, v => v >= 0 && v <= 2, null), sel = st ?? defCodec ?? 0;
    const sup = sel === 0 ? support.av1 : sel === 1 ? support.h265 : support.h264;
    codecSel.value = sup ? sel : (support.av1 ? 0 : support.h265 ? 1 : 2); S.currentCodec = +codecSel.value;
};

const tabStrip = $('tabStrip'), tabCont = $('tabContainer'), tabBtn = $('tabbedModeBtn'), tabBack = $('tabBack');
const monIco = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="3" width="20" height="14" rx="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>`;
const starIco = `<svg class="primary-star" viewBox="0 0 24 24" fill="currentColor"><path d="M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z"/></svg>`;

const rendTabs = () => {
    if (!S.monitors.length) { tabCont.innerHTML = ''; return; }
    tabCont.innerHTML = S.monitors.map(m => `<button class="tab-item${m.index === S.currentMon ? ' active' : ''}" data-index="${m.index}">${monIco}<div class="tab-item-info"><span class="tab-item-name">${m.name || `Display ${m.index + 1}`}${m.isPrimary ? starIco : ''}</span><span class="tab-item-res">(${m.width}x${m.height})</span></div></button>`).join('');
    tabCont.querySelectorAll('.tab-item').forEach(t => { t.onclick = () => { const i = +t.dataset.index; if (i !== S.currentMon && sendMonFn) sendMonFn(i); }; });
};

const updTabUI = () => { tabBtn.classList.toggle('on', S.tabbedMode); document.body.classList.toggle('tabbed-mode', S.tabbedMode); const sh = S.tabbedMode && S.monitors.length > 0; tabStrip.classList.toggle('visible', sh); if (sh) rendTabs(); setTimeout(() => window.dispatchEvent(new Event('resize')), 350); };

S.tabbedMode = localStorage.getItem(K.TAB) === 'true';
tabBtn.onclick = () => { S.tabbedMode = !S.tabbedMode; svPref(K.TAB, S.tabbedMode); updTabUI(); };
tabBack.onclick = () => { S.tabbedMode = 0; svPref(K.TAB, 0); updTabUI(); };

if (S.tabbedMode) { tabBtn.classList.add('on'); document.body.classList.add('tabbed-mode'); if (S.monitors.length > 0) { tabStrip.classList.add('visible'); rendTabs(); } }

export const updateMonOpts = () => {
    monSel.innerHTML = S.monitors.length ? S.monitors.map(m => `<option value="${m.index}">#${m.index + 1}${m.isPrimary ? '*' : ''} ${m.width}x${m.height}@${m.refreshRate}</option>`).join('') : '<option value="0">Waiting...</option>';
    monSel.value = S.currentMon;
    if (S.tabbedMode) { const sh = S.monitors.length > 0; tabStrip.classList.toggle('visible', sh); if (sh) rendTabs(); }
};

// Stats overlay - data-driven approach
const statsOv = $('statsOverlay'), statsTog = $('statsToggleBtn');
let statsEn = localStorage.getItem(K.STA) === 'true', unsubMet = null;

const fmt = (v, d = 2, s = '') => v > 0 ? `${v.toFixed(d)}${s}` : `--${s}`;

const updStats = d => {
    if (!statsEn) return;
    const { stats, jitter, clock, network, decode, render, audio, session, computed } = d;
    const cn = ['AV1', 'H.265', 'H.264'][S.currentCodec] || 'H.264';

    // Simple direct updates - much more maintainable than 30+ individual lines
    const u = {
        statsFps: `${computed.fps}/${computed.targetFps} (${computed.fpsEff.toFixed(0)}%)`,
        statsBitrate: `${computed.mbps} Mbps`,
        statsResolution: S.W > 0 ? `${S.W}x${S.H}` : '--x--',
        statsCodec: `${cn} (${S.hwAccel})`,
        statsRtt: clock.valid ? fmt(clock.rttMs, 1, ' ms') : '-- ms',
        statsFrameAge: fmt(jitter.avgServerAgeMs, 1, ' ms'),
        statsInterval: fmt(jitter.intervalMean, 1, ' ms'),
        statsStdDev: fmt(jitter.intervalStdDev, 2, ' ms'),
        statsDecodeTime: fmt(decode.avgDecodeTimeMs, 2, ' ms'),
        statsDecodeQueue: decode.maxQueueSize.toString(),
        statsHwAccel: S.hwAccel || '--',
        statsRenderTime: fmt(render.avgRenderTimeMs, 2, ' ms'),
        statsRenderFrames: render.renderCount.toString(),
        statsPackets: `${network.packetsReceived} (V:${network.videoPackets} A:${network.audioPackets})`,
        statsPacketSize: network.avgPacketSize > 0 ? `${network.avgPacketSize.toFixed(0)} B` : '-- B',
        statsAudioPackets: audio.packetsReceived.toString(),
        statsAudioDecoded: audio.packetsDecoded.toString(),
        statsAudioBuffer: fmt(audio.avgBufferHealth, 1, ' ms'),
        statsInputMouse: `${stats.moves} moves, ${stats.clicks} clicks`,
        statsInputKeys: stats.keys.toString(),
        statsUptime: `${d.uptime}s`,
        statsTotalFrames: session.totalFrames.toLocaleString(),
        statsTotalData: `${(session.totalBytes / 1048576).toFixed(2)} MB`
    };
    for (const [id, val] of Object.entries(u)) { const el = $(id); if (el) el.textContent = val; }

    // RTT highlighting
    const rtt = $('statsRtt');
    if (rtt) rtt.parentElement.className = 'stats-row' + (clock.rttMs > 100 ? ' error' : clock.rttMs > 50 ? ' warn' : clock.rttMs > 0 ? ' highlight' : '');

    // Drops sections
    const tad = audio.packetsDropped + audio.bufferUnderruns + audio.bufferOverflows, ads = $('statsAudioDropsSection');
    if (ads) { ads.classList.toggle('has-drops', tad > 0); if (tad > 0) { $('statsAudioDropped').textContent = audio.packetsDropped; $('statsAudioUnderruns').textContent = audio.bufferUnderruns; $('statsAudioOverflows').textContent = audio.bufferOverflows; } }
    const td = stats.framesDropped + stats.framesTimeout + jitter.framesDroppedLate + stats.decodeErrors, ds = $('statsDropsSection');
    if (ds) { ds.classList.toggle('has-drops', td > 0); if (td > 0) { $('statsDropsDropped').textContent = stats.framesDropped; $('statsDropsTimeout').textContent = stats.framesTimeout; $('statsDropsLate').textContent = jitter.framesDroppedLate; $('statsDecodeErrors').textContent = stats.decodeErrors; } }
};

const updStatsVis = () => { statsOv.classList.toggle('visible', statsEn); statsTog.classList.toggle('on', statsEn); if (statsEn && !unsubMet) unsubMet = subscribeToMetrics(updStats); else if (!statsEn && unsubMet) { unsubMet(); unsubMet = null; } };
statsTog.onclick = () => { statsEn = !statsEn; svPref(K.STA, statsEn); updStatsVis(); };
updStatsVis();

// Toggle button helper
const mkToggle = (btnId, hintId, stateKey, onChange) => {
    const btn = $(btnId), hnt = hintId ? $(hintId) : null;
    let en = stateKey ? localStorage.getItem(stateKey) === 'true' : 0;
    const upd = () => { btn.classList.toggle('on', en); hnt?.classList.toggle('visible', en); onChange?.(en); };
    btn.onclick = () => { en = !en; if (stateKey) svPref(stateKey, en); upd(); };
    upd();
    return { get: () => en, set: v => { en = v; upd(); } };
};

const relMouse = mkToggle('relativeMouseBtn', 'relativeMouseHint', null, setRelativeMouseMode);
window.addEventListener('pointerlockchange', e => { if (e.detail?.relativeMouseDisabled) relMouse.set(0); });

S.clipboardSyncEnabled = localStorage.getItem(K.CLI) === 'true';
mkToggle('clipboardSyncBtn', 'clipboardSyncHint', K.CLI, en => { S.clipboardSyncEnabled = en; });
