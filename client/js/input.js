import { MSG, S, mkBuf, log, safe } from './state.js';
import { canvas, canvasW, canvasH, calcVp, resetCursorStyle } from './renderer.js';

const BMAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };
const mAbsBuf = new ArrayBuffer(12), mAbsV = new DataView(mAbsBuf);
const mRelBuf = new ArrayBuffer(8), mRelV = new DataView(mRelBuf);
mAbsV.setUint32(0, MSG.MOUSE_MOVE, 1);
mRelV.setUint32(0, MSG.MOUSE_MOVE_REL, 1);

let pendAbs = null, pendRel = { dx: 0, dy: 0 }, rafId = null, clipReqFn = null, curCapFn = null;

export const setClipboardRequestFn = fn => { clipReqFn = fn; };
export const setCursorCaptureFn = fn => { curCapFn = fn; };

const CM = {Backspace:8,Tab:9,Enter:13,ShiftLeft:16,ShiftRight:16,ControlLeft:17,ControlRight:17,AltLeft:18,AltRight:18,Pause:19,CapsLock:20,Escape:27,Space:32,PageUp:33,PageDown:34,End:35,Home:36,ArrowLeft:37,ArrowUp:38,ArrowRight:39,ArrowDown:40,PrintScreen:44,Insert:45,Delete:46,MetaLeft:91,MetaRight:92,ContextMenu:93,Numpad0:96,Numpad1:97,Numpad2:98,Numpad3:99,Numpad4:100,Numpad5:101,Numpad6:102,Numpad7:103,Numpad8:104,Numpad9:105,NumpadMultiply:106,NumpadAdd:107,NumpadSubtract:109,NumpadDecimal:110,NumpadDivide:111,F1:112,F2:113,F3:114,F4:115,F5:116,F6:117,F7:118,F8:119,F9:120,F10:121,F11:122,F12:123,NumLock:144,ScrollLock:145,Semicolon:186,Equal:187,Comma:188,Minus:189,Period:190,Slash:191,Backquote:192,BracketLeft:219,Backslash:220,BracketRight:221,Quote:222};
const codeToVK = c => c.startsWith('Key') && c.length === 4 ? c.charCodeAt(3) : c.startsWith('Digit') && c.length === 6 ? c.charCodeAt(5) : CM[c] || 0;

const sendNow = (t, ...a) => {
    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') return;
    const mk = {
        btn: () => { S.stats.clicks++; return mkBuf(6, v => { v.setUint32(0, MSG.MOUSE_BTN, 1); v.setUint8(4, a[0]); v.setUint8(5, a[1] ? 1 : 0); }); },
        wheel: () => mkBuf(8, v => { v.setUint32(0, MSG.MOUSE_WHEEL, 1); v.setInt16(4, Math.round(a[0]), 1); v.setInt16(6, Math.round(a[1]), 1); }),
        key: () => { S.stats.keys++; return mkBuf(10, v => { v.setUint32(0, MSG.KEY, 1); v.setUint16(4, a[0], 1); v.setUint16(6, a[1], 1); v.setUint8(8, a[2] ? 1 : 0); v.setUint8(9, a[3]); }); }
    };
    const buf = mk[t]?.();
    if (buf) safe(() => S.dcInput.send(buf), log.warn('INPUT', 'Send fail', { t }));
};

const flush = () => {
    rafId = null;
    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') { pendAbs = null; pendRel.dx = pendRel.dy = 0; return; }
    if (pendRel.dx || pendRel.dy) {
        S.stats.moves++;
        mRelV.setInt16(4, pendRel.dx, 1); mRelV.setInt16(6, pendRel.dy, 1);
        safe(() => S.dcInput.send(mRelBuf));
        pendRel.dx = pendRel.dy = 0;
    }
    if (pendAbs) {
        S.stats.moves++;
        mAbsV.setFloat32(4, pendAbs.x, 1); mAbsV.setFloat32(8, pendAbs.y, 1);
        safe(() => S.dcInput.send(mAbsBuf));
        pendAbs = null;
    }
};

const sched = () => { if (rafId === null) rafId = requestAnimationFrame(flush); };
const qAbs = (x, y) => { pendAbs = { x, y }; sched(); };
const qRel = (dx, dy) => { pendRel.dx = Math.max(-32768, Math.min(32767, pendRel.dx + dx)); pendRel.dy = Math.max(-32768, Math.min(32767, pendRel.dy + dy)); sched(); };

const toNorm = (cx, cy) => {
    if (S.W <= 0 || S.H <= 0 || !canvas) return null;
    const rect = canvas.getBoundingClientRect(), dpr = devicePixelRatio || 1;
    const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH);
    const x = (cx - rect.left) * dpr, y = (cy - rect.top) * dpr;
    if (x < vp.x || x > vp.x + vp.w || y < vp.y || y > vp.y + vp.h) return null;
    return { x: Math.max(0, Math.min(1, (x - vp.x) / vp.w)), y: Math.max(0, Math.min(1, (y - vp.y) / vp.h)) };
};

const getMods = e => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);
const isInpFocus = () => { const el = document.activeElement; return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable); };

const keyH = (e, down) => {
    if (!S.controlEnabled || isInpFocus()) return;
    if (!e.metaKey) e.preventDefault();
    const mods = getMods(e), vk = codeToVK(e.code);
    if (!vk) return;
    if (down && e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey && (e.code === 'KeyV' || e.code === 'KeyC')) {
        sendNow('key', vk, 0, 1, mods);
        if (S.clipboardSyncEnabled && clipReqFn) setTimeout(clipReqFn, e.code === 'KeyV' ? 100 : 150);
        return;
    }
    sendNow('key', vk, 0, down, mods);
};

const isPtrLock = () => document.pointerLockElement === canvas;
const exitPtrLock = () => { if (isPtrLock()) document.exitPointerLock?.(); };

document.addEventListener('pointerlockchange', () => {
    S.pointerLocked = isPtrLock();
    if (!S.pointerLocked) S.relativeMouseMode = 0;
    log.debug('INPUT', 'Ptr lock', { locked: S.pointerLocked });
    window.dispatchEvent(new CustomEvent('pointerlockchange', { detail: { locked: S.pointerLocked, relativeMouseDisabled: !S.pointerLocked } }));
});

const h = {
    move: e => {
        if (!S.controlEnabled) return;
        if (S.relativeMouseMode && S.pointerLocked) { const dx = Math.round(e.movementX), dy = Math.round(e.movementY); if (dx || dy) qRel(dx, dy); }
        else if (!S.relativeMouseMode) { const p = toNorm(e.clientX, e.clientY); if (p) qAbs(p.x, p.y); }
    },
    down: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();
        if (rafId !== null) { cancelAnimationFrame(rafId); flush(); }
        if (S.relativeMouseMode && !S.pointerLocked) safe(() => canvas.requestPointerLock?.());
        sendNow('btn', BMAP[e.button] ?? 0, 1);
    },
    up: e => { if (!S.controlEnabled) return; e.preventDefault(); if (rafId !== null) { cancelAnimationFrame(rafId); flush(); } sendNow('btn', BMAP[e.button] ?? 0, 0); },
    wheel: e => { if (S.controlEnabled) { e.preventDefault(); sendNow('wheel', e.deltaX, e.deltaY); } },
    ctx: e => { if (S.controlEnabled) e.preventDefault(); },
    keyD: e => keyH(e, 1),
    keyU: e => keyH(e, 0)
};

const toggleCtrl = en => {
    if (en === S.controlEnabled || !canvas) return;
    S.controlEnabled = en;
    const m = en ? 'addEventListener' : 'removeEventListener';
    canvas[m]('mousemove', h.move); canvas[m]('mousedown', h.down); canvas[m]('mouseup', h.up);
    canvas[m]('contextmenu', h.ctx); canvas[m]('wheel', h.wheel, { passive: 0 });
    document[m]('keydown', h.keyD); document[m]('keyup', h.keyU);
    log.info('INPUT', en ? 'Enabled' : 'Disabled');
    if (!en) { if (rafId !== null) { cancelAnimationFrame(rafId); rafId = null; } pendAbs = null; pendRel.dx = pendRel.dy = 0; if (S.pointerLocked) exitPtrLock(); }
};

export const setRelativeMouseMode = en => {
    S.relativeMouseMode = en;
    curCapFn?.(en);
    if (en) resetCursorStyle();
    if (!en && S.pointerLocked) exitPtrLock();
};

export const enableControl = () => toggleCtrl(1);

if (window.matchMedia('(pointer: fine)').matches) enableControl();
