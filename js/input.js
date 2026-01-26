import { MSG, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp } from './renderer.js';

const BUTTON_MAP = { 0: 0, 2: 1, 1: 2, 3: 3, 4: 4 };

let isKeyboardLockedFn = () => false;
let exitFullscreenFn = () => {};

export const setKeyboardLockFns = (lockCheckFn, exitFn) => {
    isKeyboardLockedFn = lockCheckFn;
    exitFullscreenFn = exitFn;
};

const send = (type, ...args) => {
    if (!S.controlEnabled || S.dc?.readyState !== 'open') return;

    let buf;
    switch (type) {
        case 'move':
            S.stats.moves++;
            buf = mkBuf(12, v => {
                v.setUint32(0, MSG.MOUSE_MOVE, true);
                v.setFloat32(4, args[0], true);
                v.setFloat32(8, args[1], true);
            });
            break;
        case 'btn':
            S.stats.clicks++;
            buf = mkBuf(6, v => {
                v.setUint32(0, MSG.MOUSE_BTN, true);
                v.setUint8(4, args[0]);
                v.setUint8(5, args[1] ? 1 : 0);
            });
            break;
        case 'wheel':
            buf = mkBuf(8, v => {
                v.setUint32(0, MSG.MOUSE_WHEEL, true);
                v.setInt16(4, Math.round(args[0]), true);
                v.setInt16(6, Math.round(args[1]), true);
            });
            break;
        case 'key':
            S.stats.keys++;
            buf = mkBuf(10, v => {
                v.setUint32(0, MSG.KEY, true);
                v.setUint16(4, args[0], true);
                v.setUint16(6, args[1], true);
                v.setUint8(8, args[2] ? 1 : 0);
                v.setUint8(9, args[3]);
            });
            break;
        default: return;
    }
    try { S.dc.send(buf); } catch {}
};

const toNormalized = (clientX, clientY) => {
    if (S.W <= 0 || S.H <= 0) return null;

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

const getModifiers = e => {
    let mods = 0;
    if (e.ctrlKey) mods |= 1;
    if (e.altKey) mods |= 2;
    if (e.shiftKey) mods |= 4;
    if (e.metaKey) mods |= 8;
    return mods;
};

const isInputFocused = () => {
    const el = document.activeElement;
    return el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable);
};

const keyHandler = (e, isDown) => {
    if (!S.controlEnabled || isInputFocused()) return;
    if (!e.metaKey) e.preventDefault();
    send('key', e.keyCode, 0, isDown, getModifiers(e));
};

const handlers = {
    move: e => {
        if (S.controlEnabled) {
            const pos = toNormalized(e.clientX, e.clientY);
            if (pos) send('move', pos.x, pos.y);
        }
    },
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
    const method = enable ? 'addEventListener' : 'removeEventListener';

    canvas[method]('mousemove', handlers.move);
    canvas[method]('mousedown', handlers.down);
    canvas[method]('mouseup', handlers.up);
    canvas[method]('contextmenu', handlers.ctx);
    canvas[method]('wheel', handlers.wheel, { passive: false });

    document[method]('keydown', handlers.keyD);
    document[method]('keyup', handlers.keyU);
};

export const enableControl = () => toggleControl(true);
export const disableControl = () => toggleControl(false);
export const isDesktop = () => window.matchMedia('(pointer: fine)').matches;

if (isDesktop()) enableControl();
