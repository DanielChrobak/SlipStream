import { MSG, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp } from './renderer.js';

const BUTTON_MAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };

const send = (type, ...a) => {
    if (!S.controlEnabled || S.dc?.readyState !== 'open') return;
    const makers = {
        move: () => { S.stats.moves++; return mkBuf(12, v => { v.setUint32(0, MSG.MOUSE_MOVE, true); v.setFloat32(4, a[0], true); v.setFloat32(8, a[1], true); }); },
        btn: () => { S.stats.clicks++; return mkBuf(6, v => { v.setUint32(0, MSG.MOUSE_BTN, true); v.setUint8(4, a[0]); v.setUint8(5, a[1] ? 1 : 0); }); },
        wheel: () => mkBuf(8, v => { v.setUint32(0, MSG.MOUSE_WHEEL, true); v.setInt16(4, Math.round(a[0]), true); v.setInt16(6, Math.round(a[1]), true); }),
        key: () => { S.stats.keys++; return mkBuf(10, v => { v.setUint32(0, MSG.KEY, true); v.setUint16(4, a[0], true); v.setUint16(6, a[1], true); v.setUint8(8, a[2] ? 1 : 0); v.setUint8(9, a[3]); }); }
    };
    const buf = makers[type]?.();
    if (buf) try { S.dc.send(buf); } catch {}
};

const toNormalized = (cx, cy) => {
    if (S.W <= 0 || S.H <= 0) return null;
    const rect = canvas.getBoundingClientRect(), dpr = devicePixelRatio || 1;
    const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH);
    const x = (cx - rect.left) * dpr, y = (cy - rect.top) * dpr;
    if (x < vp.x || x > vp.x + vp.w || y < vp.y || y > vp.y + vp.h) return null;
    return { x: Math.max(0, Math.min(1, (x - vp.x) / vp.w)), y: Math.max(0, Math.min(1, (y - vp.y) / vp.h)) };
};

const getMods = e => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);
const isInputFocused = () => { const el = document.activeElement; return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable); };

const keyHandler = (e, down) => {
    if (!S.controlEnabled || isInputFocused()) return;
    if (!e.metaKey) e.preventDefault();
    send('key', e.keyCode, 0, down, getMods(e));
};

const h = {
    move: e => { if (S.controlEnabled) { const p = toNormalized(e.clientX, e.clientY); if (p) send('move', p.x, p.y); } },
    down: e => { if (S.controlEnabled) { e.preventDefault(); send('btn', BUTTON_MAP[e.button] ?? 0, true); } },
    up: e => { if (S.controlEnabled) { e.preventDefault(); send('btn', BUTTON_MAP[e.button] ?? 0, false); } },
    wheel: e => { if (S.controlEnabled) { e.preventDefault(); send('wheel', e.deltaX, e.deltaY); } },
    ctx: e => { if (S.controlEnabled) e.preventDefault(); },
    keyD: e => keyHandler(e, true),
    keyU: e => keyHandler(e, false)
};

const toggleControl = enable => {
    if (enable === S.controlEnabled) return;
    S.controlEnabled = enable;
    const m = enable ? 'addEventListener' : 'removeEventListener';
    canvas[m]('mousemove', h.move);
    canvas[m]('mousedown', h.down);
    canvas[m]('mouseup', h.up);
    canvas[m]('contextmenu', h.ctx);
    canvas[m]('wheel', h.wheel, { passive: false });
    document[m]('keydown', h.keyD);
    document[m]('keyup', h.keyU);
};

export const enableControl = () => toggleControl(true);
export const disableControl = () => toggleControl(false);
export const isDesktop = () => window.matchMedia('(pointer: fine)').matches;

if (isDesktop()) enableControl();
