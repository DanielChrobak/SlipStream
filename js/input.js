import { MSG, S, mkBuf } from './state.js';
import { canvas, canvasW, canvasH, calcVp } from './renderer.js';

// Button mapping (browser button codes to protocol)
const BUTTON_MAP = {
    0: 0, // Left
    2: 1, // Right
    1: 2, // Middle
    3: 3, // Back
    4: 4  // Forward
};

// Keyboard lock state handlers
let isKeyboardLockedFn = () => false;
let exitFullscreenFn = () => {};

export const setKeyboardLockFns = (lockCheckFn, exitFn) => {
    isKeyboardLockedFn = lockCheckFn;
    exitFullscreenFn = exitFn;
};

// Send input message over data channel
const send = (type, ...args) => {
    if (!S.controlEnabled || S.dc?.readyState !== 'open') {
        return;
    }

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
                v.setUint16(4, args[0], true);  // keyCode
                v.setUint16(6, args[1], true);  // scanCode (unused, 0)
                v.setUint8(8, args[2] ? 1 : 0); // isDown
                v.setUint8(9, args[3]);         // modifiers
            });
            break;

        default:
            return;
    }

    try {
        S.dc.send(buf);
    } catch {}
};

// Convert client coordinates to normalized viewport coordinates
const toNormalized = (clientX, clientY) => {
    if (S.W <= 0 || S.H <= 0) {
        return null;
    }

    const rect = canvas.getBoundingClientRect();
    const dpr = devicePixelRatio || 1;
    const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH);

    const x = (clientX - rect.left) * dpr;
    const y = (clientY - rect.top) * dpr;

    // Check if within viewport bounds
    if (x < vp.x || x > vp.x + vp.w || y < vp.y || y > vp.y + vp.h) {
        return null;
    }

    return {
        x: Math.max(0, Math.min(1, (x - vp.x) / vp.w)),
        y: Math.max(0, Math.min(1, (y - vp.y) / vp.h))
    };
};

// Get modifier key flags
const getModifiers = e => {
    let mods = 0;
    if (e.ctrlKey) mods |= 1;
    if (e.altKey) mods |= 2;
    if (e.shiftKey) mods |= 4;
    if (e.metaKey) mods |= 8;
    return mods;
};

// Check if focus is on an input element
const isInputFocused = () => {
    const el = document.activeElement;
    return el && (
        el.tagName === 'INPUT' ||
        el.tagName === 'TEXTAREA' ||
        el.isContentEditable
    );
};

// Keyboard event handler
const keyHandler = (e, isDown) => {
    if (!S.controlEnabled || isInputFocused()) {
        return;
    }

    // Allow meta key combinations to pass through
    if (!e.metaKey) {
        e.preventDefault();
    }

    send('key', e.keyCode, 0, isDown, getModifiers(e));
};

// Event handlers
const handlers = {
    move: e => {
        if (S.controlEnabled) {
            const pos = toNormalized(e.clientX, e.clientY);
            if (pos) {
                send('move', pos.x, pos.y);
            }
        }
    },

    down: e => {
        if (S.controlEnabled) {
            e.preventDefault();
            send('btn', BUTTON_MAP[e.button] ?? 0, true);
        }
    },

    up: e => {
        if (S.controlEnabled) {
            e.preventDefault();
            send('btn', BUTTON_MAP[e.button] ?? 0, false);
        }
    },

    wheel: e => {
        if (S.controlEnabled) {
            e.preventDefault();
            send('wheel', e.deltaX, e.deltaY);
        }
    },

    ctx: e => {
        if (S.controlEnabled) {
            e.preventDefault();
        }
    },

    keyD: e => keyHandler(e, true),
    keyU: e => keyHandler(e, false)
};

// Toggle control capture
const toggleControl = enable => {
    if (enable === S.controlEnabled) {
        return;
    }

    S.controlEnabled = enable;
    const method = enable ? 'addEventListener' : 'removeEventListener';

    // Canvas mouse events
    canvas[method]('mousemove', handlers.move);
    canvas[method]('mousedown', handlers.down);
    canvas[method]('mouseup', handlers.up);
    canvas[method]('contextmenu', handlers.ctx);
    canvas[method]('wheel', handlers.wheel, { passive: false });

    // Document keyboard events
    document[method]('keydown', handlers.keyD);
    document[method]('keyup', handlers.keyU);
};

// Public control functions
export const enableControl = () => toggleControl(true);
export const disableControl = () => toggleControl(false);

// Check if device is desktop (has precise pointer)
export const isDesktop = () => window.matchMedia('(pointer: fine)').matches;

// Auto-enable control on desktop
if (isDesktop()) {
    enableControl();
}
