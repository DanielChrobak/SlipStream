import { MSG, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp } from './renderer.js';

const BUTTON_MAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };
const moveAbsBuf = new ArrayBuffer(12), moveAbsView = new DataView(moveAbsBuf);
const moveRelBuf = new ArrayBuffer(8), moveRelView = new DataView(moveRelBuf);
moveAbsView.setUint32(0, MSG.MOUSE_MOVE, true);
moveRelView.setUint32(0, MSG.MOUSE_MOVE_REL, true);

let pendingAbsMove = null, pendingRelMove = { dx: 0, dy: 0 }, rafId = null;

const sendImmediate = (type, ...a) => {
    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') return;
    const makers = {
        btn: () => { S.stats.clicks++; return mkBuf(6, v => { v.setUint32(0, MSG.MOUSE_BTN, true); v.setUint8(4, a[0]); v.setUint8(5, a[1] ? 1 : 0); }); },
        wheel: () => mkBuf(8, v => { v.setUint32(0, MSG.MOUSE_WHEEL, true); v.setInt16(4, Math.round(a[0]), true); v.setInt16(6, Math.round(a[1]), true); }),
        key: () => { S.stats.keys++; return mkBuf(10, v => { v.setUint32(0, MSG.KEY, true); v.setUint16(4, a[0], true); v.setUint16(6, a[1], true); v.setUint8(8, a[2] ? 1 : 0); v.setUint8(9, a[3]); }); }
    };
    const buf = makers[type]?.();
    if (buf) try { S.dcInput.send(buf); } catch {}
};

const flushMouseState = () => {
    rafId = null;
    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') {
        pendingAbsMove = null; pendingRelMove.dx = pendingRelMove.dy = 0; return;
    }
    if (pendingRelMove.dx !== 0 || pendingRelMove.dy !== 0) {
        S.stats.moves++;
        moveRelView.setInt16(4, pendingRelMove.dx, true);
        moveRelView.setInt16(6, pendingRelMove.dy, true);
        try { S.dcInput.send(moveRelBuf); } catch {}
        pendingRelMove.dx = pendingRelMove.dy = 0;
    }
    if (pendingAbsMove !== null) {
        S.stats.moves++;
        moveAbsView.setFloat32(4, pendingAbsMove.x, true);
        moveAbsView.setFloat32(8, pendingAbsMove.y, true);
        try { S.dcInput.send(moveAbsBuf); } catch {}
        pendingAbsMove = null;
    }
};

const scheduleFlush = () => { if (rafId === null) rafId = requestAnimationFrame(flushMouseState); };
const queueAbsMove = (x, y) => { pendingAbsMove = { x, y }; scheduleFlush(); };
const queueRelMove = (dx, dy) => {
    pendingRelMove.dx = Math.max(-32768, Math.min(32767, pendingRelMove.dx + dx));
    pendingRelMove.dy = Math.max(-32768, Math.min(32767, pendingRelMove.dy + dy));
    scheduleFlush();
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
const keyHandler = (e, down) => { if (!S.controlEnabled || isInputFocused()) return; if (!e.metaKey) e.preventDefault(); sendImmediate('key', e.keyCode, 0, down, getMods(e)); };
const isPointerLocked = () => document.pointerLockElement === canvas;
const exitPointerLock = () => { if (isPointerLocked()) document.exitPointerLock?.(); };

document.addEventListener('pointerlockchange', () => {
    S.pointerLocked = isPointerLocked();
    if (!S.pointerLocked) S.relativeMouseMode = false;
    window.dispatchEvent(new CustomEvent('pointerlockchange', { detail: { locked: S.pointerLocked, relativeMouseDisabled: !S.pointerLocked } }));
});

const h = {
    move: e => {
        if (!S.controlEnabled) return;
        if (S.relativeMouseMode && S.pointerLocked) {
            const dx = Math.round(e.movementX), dy = Math.round(e.movementY);
            if (dx !== 0 || dy !== 0) queueRelMove(dx, dy);
        } else if (!S.relativeMouseMode) {
            const p = toNormalized(e.clientX, e.clientY);
            if (p) queueAbsMove(p.x, p.y);
        }
    },
    down: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();
        if (rafId !== null) { cancelAnimationFrame(rafId); flushMouseState(); }
        if (S.relativeMouseMode && !S.pointerLocked) try { canvas.requestPointerLock?.(); } catch {}
        sendImmediate('btn', BUTTON_MAP[e.button] ?? 0, true);
    },
    up: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();
        if (rafId !== null) { cancelAnimationFrame(rafId); flushMouseState(); }
        sendImmediate('btn', BUTTON_MAP[e.button] ?? 0, false);
    },
    wheel: e => { if (!S.controlEnabled) return; e.preventDefault(); sendImmediate('wheel', e.deltaX, e.deltaY); },
    ctx: e => { if (S.controlEnabled) e.preventDefault(); },
    keyD: e => keyHandler(e, true),
    keyU: e => keyHandler(e, false)
};

const toggleControl = enable => {
    if (enable === S.controlEnabled) return;
    S.controlEnabled = enable;
    const m = enable ? 'addEventListener' : 'removeEventListener';
    canvas[m]('mousemove', h.move); canvas[m]('mousedown', h.down); canvas[m]('mouseup', h.up);
    canvas[m]('contextmenu', h.ctx); canvas[m]('wheel', h.wheel, { passive: false });
    document[m]('keydown', h.keyD); document[m]('keyup', h.keyU);
    if (!enable) {
        if (rafId !== null) { cancelAnimationFrame(rafId); rafId = null; }
        pendingAbsMove = null; pendingRelMove.dx = pendingRelMove.dy = 0;
        if (S.pointerLocked) exitPointerLock();
    }
};

export const setRelativeMouseMode = enabled => { S.relativeMouseMode = enabled; if (!enabled && S.pointerLocked) exitPointerLock(); };
export const enableControl = () => toggleControl(true);

if (window.matchMedia('(pointer: fine)').matches) enableControl();
