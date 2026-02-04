import { MSG, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp, resetCursorStyle } from './renderer.js';

const BUTTON_MAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };
const moveAbsBuf = new ArrayBuffer(12), moveAbsView = new DataView(moveAbsBuf);
const moveRelBuf = new ArrayBuffer(8), moveRelView = new DataView(moveRelBuf);
moveAbsView.setUint32(0, MSG.MOUSE_MOVE, true);
moveRelView.setUint32(0, MSG.MOUSE_MOVE_REL, true);

let pendingAbsMove = null, pendingRelMove = { dx: 0, dy: 0 }, rafId = null;
let clipboardSendFn = null, clipboardRequestFn = null, sendCursorCaptureFn = null;

export const setClipboardFns = (sendFn, requestFn) => { clipboardSendFn = sendFn; clipboardRequestFn = requestFn; };
export const setCursorCaptureFn = fn => { sendCursorCaptureFn = fn; };

const CODE_MAP = {
    Backspace:8,Tab:9,Enter:13,ShiftLeft:16,ShiftRight:16,ControlLeft:17,ControlRight:17,AltLeft:18,AltRight:18,
    Pause:19,CapsLock:20,Escape:27,Space:32,PageUp:33,PageDown:34,End:35,Home:36,ArrowLeft:37,ArrowUp:38,
    ArrowRight:39,ArrowDown:40,PrintScreen:44,Insert:45,Delete:46,MetaLeft:91,MetaRight:92,ContextMenu:93,
    Numpad0:96,Numpad1:97,Numpad2:98,Numpad3:99,Numpad4:100,Numpad5:101,Numpad6:102,Numpad7:103,Numpad8:104,
    Numpad9:105,NumpadMultiply:106,NumpadAdd:107,NumpadSubtract:109,NumpadDecimal:110,NumpadDivide:111,
    F1:112,F2:113,F3:114,F4:115,F5:116,F6:117,F7:118,F8:119,F9:120,F10:121,F11:122,F12:123,
    NumLock:144,ScrollLock:145,Semicolon:186,Equal:187,Comma:188,Minus:189,Period:190,Slash:191,
    Backquote:192,BracketLeft:219,Backslash:220,BracketRight:221,Quote:222
};
const codeToVK = c => c.startsWith('Key') && c.length === 4 ? c.charCodeAt(3) : c.startsWith('Digit') && c.length === 6 ? c.charCodeAt(5) : CODE_MAP[c] || 0;

const sendImmediate = (type, ...a) => {
    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') return;
    const makers = {
        btn: () => { S.stats.clicks++; return mkBuf(6, v => { v.setUint32(0, MSG.MOUSE_BTN, true); v.setUint8(4, a[0]); v.setUint8(5, a[1] ? 1 : 0); }); },
        wheel: () => mkBuf(8, v => { v.setUint32(0, MSG.MOUSE_WHEEL, true); v.setInt16(4, Math.round(a[0]), true); v.setInt16(6, Math.round(a[1]), true); }),
        key: () => { S.stats.keys++; return mkBuf(10, v => { v.setUint32(0, MSG.KEY, true); v.setUint16(4, a[0], true); v.setUint16(6, a[1], true); v.setUint8(8, a[2] ? 1 : 0); v.setUint8(9, a[3]); }); }
    };
    const buf = makers[type]?.();
    if (buf) try { S.dcInput.send(buf); } catch (e) { console.warn('[Input] Failed to send input:', e.message); }
};

const flushMouseState = () => {
    rafId = null;
    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') { pendingAbsMove = null; pendingRelMove.dx = pendingRelMove.dy = 0; return; }
    if (pendingRelMove.dx !== 0 || pendingRelMove.dy !== 0) {
        S.stats.moves++;
        moveRelView.setInt16(4, pendingRelMove.dx, true);
        moveRelView.setInt16(6, pendingRelMove.dy, true);
        try { S.dcInput.send(moveRelBuf); } catch (e) { console.warn('[Input] Failed to send relative mouse:', e.message); }
        pendingRelMove.dx = pendingRelMove.dy = 0;
    }
    if (pendingAbsMove !== null) {
        S.stats.moves++;
        moveAbsView.setFloat32(4, pendingAbsMove.x, true);
        moveAbsView.setFloat32(8, pendingAbsMove.y, true);
        try { S.dcInput.send(moveAbsBuf); } catch (e) { console.warn('[Input] Failed to send absolute mouse:', e.message); }
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

const handleClipboardPaste = async (mods, vk) => {
    if (S.clipboardSyncEnabled && clipboardSendFn) {
        try { const text = await navigator.clipboard.readText(); if (text) await clipboardSendFn(text); } catch (e) { console.warn('[Input] Clipboard read failed:', e.message); }
    }
    sendImmediate('key', vk, 0, true, mods);
};

const handleClipboardCopy = (mods, vk) => {
    sendImmediate('key', vk, 0, true, mods);
    if (S.clipboardSyncEnabled && clipboardRequestFn) setTimeout(() => clipboardRequestFn(), 150);
};

const keyHandler = (e, down) => {
    if (!S.controlEnabled || isInputFocused()) return;
    if (!e.metaKey) e.preventDefault();
    const mods = getMods(e), vk = codeToVK(e.code);
    if (!vk) return;
    if (down && e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
        if (e.code === 'KeyV') { handleClipboardPaste(mods, vk); return; }
        if (e.code === 'KeyC') { handleClipboardCopy(mods, vk); return; }
    }
    sendImmediate('key', vk, 0, down, mods);
};

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
        if (S.relativeMouseMode && !S.pointerLocked) try { canvas.requestPointerLock?.(); } catch (err) { console.warn('[Input] Pointer lock request failed:', err.message); }
        sendImmediate('btn', BUTTON_MAP[e.button] ?? 0, true);
    },
    up: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();
        if (rafId !== null) { cancelAnimationFrame(rafId); flushMouseState(); }
        sendImmediate('btn', BUTTON_MAP[e.button] ?? 0, false);
    },
    wheel: e => { if (S.controlEnabled) { e.preventDefault(); sendImmediate('wheel', e.deltaX, e.deltaY); } },
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

export const setRelativeMouseMode = enabled => {
    S.relativeMouseMode = enabled;
    if (sendCursorCaptureFn) sendCursorCaptureFn(enabled);
    if (enabled) resetCursorStyle();
    if (!enabled && S.pointerLocked) exitPointerLock();
};

export const enableControl = () => toggleControl(true);

if (window.matchMedia('(pointer: fine)').matches) enableControl();
