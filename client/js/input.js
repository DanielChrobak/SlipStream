
import { MSG, S, mkBuf, log, safe } from './state.js';
import { canvas, canvasW, canvasH, calcVp, resetCursorStyle } from './renderer.js';
const BUTTON_MAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };
const mAbsBuf = new ArrayBuffer(12), mAbsView = new DataView(mAbsBuf);
const mRelBuf = new ArrayBuffer(8), mRelView = new DataView(mRelBuf);
mAbsView.setUint32(0, MSG.MOUSE_MOVE, 1);
mRelView.setUint32(0, MSG.MOUSE_MOVE_REL, 1);
let pendingAbs = null;
let pendingRel = { dx: 0, dy: 0 };
let rafId = null;
let clipboardRequestFn = null;
let clipboardPushFn = null;
let cursorCaptureFn = null;
let pendingClipboardPaste = null;

export const setClipboardRequestFn = fn => { clipboardRequestFn = fn; };
export const setClipboardPushFn = fn => { clipboardPushFn = fn; };
export const setCursorCaptureFn = fn => { cursorCaptureFn = fn; };
const CODE_MAP = {
    Backspace:8, Tab:9, Enter:13, ShiftLeft:16, ShiftRight:16,
    ControlLeft:17, ControlRight:17, AltLeft:18, AltRight:18,
    Pause:19, CapsLock:20, Escape:27, Space:32,
    PageUp:33, PageDown:34, End:35, Home:36,
    ArrowLeft:37, ArrowUp:38, ArrowRight:39, ArrowDown:40,
    PrintScreen:44, Insert:45, Delete:46,
    MetaLeft:91, MetaRight:92, ContextMenu:93,
    Numpad0:96, Numpad1:97, Numpad2:98, Numpad3:99, Numpad4:100,
    Numpad5:101, Numpad6:102, Numpad7:103, Numpad8:104, Numpad9:105,
    NumpadMultiply:106, NumpadAdd:107, NumpadSubtract:109,
    NumpadDecimal:110, NumpadDivide:111,
    F1:112, F2:113, F3:114, F4:115, F5:116, F6:117,
    F7:118, F8:119, F9:120, F10:121, F11:122, F12:123,
    NumLock:144, ScrollLock:145,
    Semicolon:186, Equal:187, Comma:188, Minus:189, Period:190,
    Slash:191, Backquote:192, BracketLeft:219, Backslash:220,
    BracketRight:221, Quote:222
};

const codeToVK = code => {
    if (code.startsWith('Key') && code.length === 4) return code.charCodeAt(3);
    if (code.startsWith('Digit') && code.length === 6) return code.charCodeAt(5);
    return CODE_MAP[code] || 0;
};
const mkBtnBuf = (btn, down) => {
    S.stats.clicks++;
    return mkBuf(6, v => {
        v.setUint32(0, MSG.MOUSE_BTN, 1);
        v.setUint8(4, btn);
        v.setUint8(5, down ? 1 : 0);
    });
};

const mkWheelBuf = (dx, dy) => mkBuf(8, v => {
    v.setUint32(0, MSG.MOUSE_WHEEL, 1);
    v.setInt16(4, Math.round(dx), 1);
    v.setInt16(6, Math.round(dy), 1);
});

const mkKeyBuf = (vk, scancode, down, mods) => {
    S.stats.keys++;
    return mkBuf(10, v => {
        v.setUint32(0, MSG.KEY, 1);
        v.setUint16(4, vk, 1);
        v.setUint16(6, scancode, 1);
        v.setUint8(8, down ? 1 : 0);
        v.setUint8(9, mods);
    });
};

const MK_MAP = { btn: mkBtnBuf, wheel: mkWheelBuf, key: mkKeyBuf };
const sendNow = (type, ...args) => {
    if (!S.controlEnabled) {
        log.debug('INPUT', 'Send blocked: control disabled', { type });
        return;
    }
    if (S.dcInput?.readyState !== 'open') {
        log.debug('INPUT', 'Send blocked: channel not open', { type, state: S.dcInput?.readyState });
        return;
    }

    const mkFn = MK_MAP[type];
    if (!mkFn) {
        log.error('INPUT', 'Unknown input type', { type });
        return;
    }

    const buf = mkFn(...args);
    const sent = safe(() => { S.dcInput.send(buf); return true; }, false, 'INPUT');
    if (!sent) {
        log.warn('INPUT', 'Send failed', { type });
    }
};
const flush = () => {
    rafId = null;

    if (!S.controlEnabled || S.dcInput?.readyState !== 'open') {
        pendingAbs = null;
        pendingRel.dx = pendingRel.dy = 0;
        return;
    }
    if (pendingRel.dx || pendingRel.dy) {
        S.stats.moves++;
        mRelView.setInt16(4, pendingRel.dx, 1);
        mRelView.setInt16(6, pendingRel.dy, 1);
        const sent = safe(() => { S.dcInput.send(mRelBuf); return true; }, false, 'INPUT');
        if (!sent) log.warn('INPUT', 'Rel move send failed');
        pendingRel.dx = pendingRel.dy = 0;
    }
    if (pendingAbs) {
        S.stats.moves++;
        mAbsView.setFloat32(4, pendingAbs.x, 1);
        mAbsView.setFloat32(8, pendingAbs.y, 1);
        const sent = safe(() => { S.dcInput.send(mAbsBuf); return true; }, false, 'INPUT');
        if (!sent) log.warn('INPUT', 'Abs move send failed');
        pendingAbs = null;
    }
};

const scheduleFlush = () => {
    if (rafId === null) rafId = requestAnimationFrame(flush);
};

const queueAbsMove = (x, y) => {
    pendingAbs = { x, y };
    scheduleFlush();
};

const queueRelMove = (dx, dy) => {
    pendingRel.dx = Math.max(-32768, Math.min(32767, pendingRel.dx + dx));
    pendingRel.dy = Math.max(-32768, Math.min(32767, pendingRel.dy + dy));
    scheduleFlush();
};
const toNormalized = (clientX, clientY) => {
    if (S.W <= 0 || S.H <= 0 || !canvas) return null;

    const rect = canvas.getBoundingClientRect();
    const dpr = devicePixelRatio || 1;
    const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH);

    const x = (clientX - rect.left) * dpr;
    const y = (clientY - rect.top) * dpr;
    if (x < vp.x || x > vp.x + vp.w || y < vp.y || y > vp.y + vp.h) return null;

    return {
        x: Math.max(0, Math.min(1, (x - vp.x) / vp.w)),
        y: Math.max(0, Math.min(1, (y - vp.y) / vp.h))
    };
};
const getModifiers = e => (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0) | (e.metaKey ? 8 : 0);
const isInputFocused = () => {
    const el = document.activeElement;
    return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable);
};
const handleKey = (e, down) => {
    if (!S.controlEnabled || isInputFocused()) return;
    if (!e.metaKey) e.preventDefault();

    const mods = getModifiers(e);
    const vk = codeToVK(e.code);

    if (!vk) {
        log.debug('INPUT', 'Unknown key code', { code: e.code });
        return;
    }

    if (!down && e.code === 'KeyV' && pendingClipboardPaste) {
        pendingClipboardPaste.keyUpQueued = true;
        return;
    }

    if (down && e.ctrlKey && !e.altKey && !e.shiftKey && !e.metaKey) {
        if (e.code === 'KeyV') {
            if (S.clipboardSyncEnabled && clipboardPushFn && !pendingClipboardPaste) {
                pendingClipboardPaste = { keyUpQueued: false };

                clipboardPushFn()
                    .catch(err => log.debug('INPUT', 'Clipboard push failed before paste', { error: err?.message }))
                    .finally(() => {
                        sendNow('key', vk, 0, 1, mods);
                        if (pendingClipboardPaste?.keyUpQueued) sendNow('key', vk, 0, 0, mods);
                        pendingClipboardPaste = null;
                        log.debug('INPUT', 'Clipboard paste shortcut handled');
                    });
                return;
            }

            sendNow('key', vk, 0, 1, mods);
            return;
        }

        if (e.code === 'KeyC') {
            sendNow('key', vk, 0, 1, mods);
            if (S.clipboardSyncEnabled && clipboardRequestFn) {
                setTimeout(clipboardRequestFn, 150);
                log.debug('INPUT', 'Clipboard shortcut', { action: 'copy' });
            }
            return;
        }
    }

    sendNow('key', vk, 0, down, mods);
};
const isPointerLocked = () => document.pointerLockElement === canvas;
const exitPointerLock = () => {
    if (isPointerLocked()) {
        document.exitPointerLock?.();
        log.debug('INPUT', 'Exiting pointer lock');
    }
};
document.addEventListener('pointerlockchange', () => {
    S.pointerLocked = isPointerLocked();
    if (!S.pointerLocked) S.relativeMouseMode = 0;

    log.info('INPUT', 'Pointer lock changed', { locked: S.pointerLocked });

    window.dispatchEvent(new CustomEvent('pointerlockchange', {
        detail: { locked: S.pointerLocked, relativeMouseDisabled: !S.pointerLocked }
    }));
});
const handlers = {
    move: e => {
        if (!S.controlEnabled) return;

        if (S.relativeMouseMode && S.pointerLocked) {
            const dx = Math.round(e.movementX);
            const dy = Math.round(e.movementY);
            if (dx || dy) queueRelMove(dx, dy);
        } else if (!S.relativeMouseMode) {
            const pos = toNormalized(e.clientX, e.clientY);
            if (pos) queueAbsMove(pos.x, pos.y);
        }
    },

    down: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();
        if (rafId !== null) { cancelAnimationFrame(rafId); flush(); }
        if (S.relativeMouseMode && !S.pointerLocked) {
            safe(() => canvas.requestPointerLock?.(), undefined, 'INPUT');
        }

        sendNow('btn', BUTTON_MAP[e.button] ?? 0, 1);
    },

    up: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();

        if (rafId !== null) { cancelAnimationFrame(rafId); flush(); }
        sendNow('btn', BUTTON_MAP[e.button] ?? 0, 0);
    },

    wheel: e => {
        if (!S.controlEnabled) return;
        e.preventDefault();
        sendNow('wheel', e.deltaX, e.deltaY);
    },

    context: e => {
        if (S.controlEnabled) e.preventDefault();
    },

    keyDown: e => handleKey(e, 1),
    keyUp: e => handleKey(e, 0)
};
const toggleControl = enable => {
    if (enable === S.controlEnabled || !canvas) return;

    S.controlEnabled = enable;
    const method = enable ? 'addEventListener' : 'removeEventListener';

    canvas[method]('mousemove', handlers.move);
    canvas[method]('mousedown', handlers.down);
    canvas[method]('mouseup', handlers.up);
    canvas[method]('contextmenu', handlers.context);
    canvas[method]('wheel', handlers.wheel, { passive: false });
    document[method]('keydown', handlers.keyDown);
    document[method]('keyup', handlers.keyUp);

    log.info('INPUT', enable ? 'Control enabled' : 'Control disabled');

    if (!enable) {
        if (rafId !== null) { cancelAnimationFrame(rafId); rafId = null; }
        pendingAbs = null;
        pendingRel.dx = pendingRel.dy = 0;
        if (S.pointerLocked) exitPointerLock();
    }
};
export const setRelativeMouseMode = enable => {
    S.relativeMouseMode = enable;
    cursorCaptureFn?.(enable);

    if (enable) {
        resetCursorStyle();
        log.info('INPUT', 'Relative mouse mode enabled');
    } else {
        if (S.pointerLocked) exitPointerLock();
        log.info('INPUT', 'Relative mouse mode disabled');
    }
};

export const enableControl = () => toggleControl(1);
if (window.matchMedia('(pointer: fine)').matches) {
    log.debug('INPUT', 'Fine pointer detected, enabling control');
    enableControl();
}
