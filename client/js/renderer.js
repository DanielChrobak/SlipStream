
import { C, S, CURSOR_TYPES, serverFrameAgeMs, recordRenderTime, logVideoDrop, log, safe } from './state.js';

export const canvas = document.querySelector('#c');
export let canvasW = 0, canvasH = 0;
const gl = canvas?.getContext('webgl2', {
    alpha: false,
    depth: false,
    stencil: false,
    antialias: false,
    desynchronized: true,
    powerPreference: 'high-performance',
    preserveDrawingBuffer: false
});

let lastViewport = null;
let hasValidTexture = false;
const BG_COLOR = [0.039, 0.039, 0.043, 1];
const updateSize = () => {
    const dpr = devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const displayW = Math.round(rect.width * dpr);
    const displayH = Math.round(rect.height * dpr);

    if (displayW > 0 && displayH > 0 && (canvas.width !== displayW || canvas.height !== displayH)) {
        canvas.width = canvasW = displayW;
        canvas.height = canvasH = displayH;
        log.debug('RENDER', 'Canvas resized', { w: canvasW, h: canvasH, dpr });
        return true;
    }
    return false;
};
const init = () => {
    if (!gl) {
        log.error('RENDER', 'WebGL2 not available', { hasCanvas: !!canvas });
        return false;
    }
    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const vs = gl.createShader(gl.VERTEX_SHADER);
    const fs = gl.createShader(gl.FRAGMENT_SHADER);

    gl.shaderSource(vs, `#version 300 es
        in vec2 a_pos, a_tex;
        out vec2 v;
        void main() { gl_Position = vec4(a_pos, 0., 1.); v = a_tex; }
    `);

    gl.shaderSource(fs, `#version 300 es
        precision highp float;
        in vec2 v;
        uniform sampler2D u;
        out vec4 o;
        void main() { o = texture(u, v); }
    `);

    gl.compileShader(vs);
    gl.compileShader(fs);
    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
        log.error('RENDER', 'Vertex shader compilation failed', { log: gl.getShaderInfoLog(vs) });
        return false;
    }
    if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
        log.error('RENDER', 'Fragment shader compilation failed', { log: gl.getShaderInfoLog(fs) });
        return false;
    }
    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);

    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
        log.error('RENDER', 'Shader program link failed', { log: gl.getProgramInfoLog(prog) });
        return false;
    }

    gl.useProgram(prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
           -1, -1, 0, 1,
            1, -1, 1, 1,
           -1,  1, 0, 0,
            1,  1, 1, 0
    ]), gl.STATIC_DRAW);
    const posLoc = gl.getAttribLocation(prog, 'a_pos');
    const texLoc = gl.getAttribLocation(prog, 'a_tex');

    gl.enableVertexAttribArray(posLoc);
    gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 16, 0);

    gl.enableVertexAttribArray(texLoc);
    gl.vertexAttribPointer(texLoc, 2, gl.FLOAT, false, 16, 8);
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.clearColor(...BG_COLOR);
    updateSize();
    gl.viewport(0, 0, canvasW, canvasH);
    gl.clear(gl.COLOR_BUFFER_BIT);

    log.info('RENDER', 'WebGL initialized');
    return true;
};
export const calcVp = (videoW, videoH, displayW, displayH) => {
    if (displayW <= 0 || displayH <= 0 || videoW <= 0 || videoH <= 0) {
        return { x: 0, y: 0, w: displayW || 1, h: displayH || 1 };
    }

    const videoAspect = videoW / videoH;
    const displayAspect = displayW / displayH;

    if (displayAspect > videoAspect) {
        const w = Math.round(displayH * videoAspect);
        return { x: Math.round((displayW - w) / 2), y: 0, w, h: displayH };
    }
    const h = Math.round(displayW / videoAspect);
    return { x: 0, y: Math.round((displayH - h) / 2), w: displayW, h };
};
const clearLetterbox = vp => {
    gl.enable(gl.SCISSOR_TEST);

    const regions = [
        [0, 0, vp.x, canvasH],
        [vp.x + vp.w, 0, canvasW - vp.x - vp.w, canvasH],
        [0, 0, canvasW, vp.y],
        [0, vp.y + vp.h, canvasW, canvasH - vp.y - vp.h]
    ];

    for (const [x, y, w, h] of regions) {
        if (w > 0 && h > 0) {
            gl.scissor(x, y, w, h);
            gl.clear(gl.COLOR_BUFFER_BIT);
        }
    }

    gl.disable(gl.SCISSOR_TEST);
};
const renderFrame = (frame, meta) => {
    const startTime = performance.now();
    const { displayWidth: vW, displayHeight: vH } = frame;

    if (!vW || !vH || vW <= 0 || vH <= 0) {
        logVideoDrop('Invalid frame dimensions', { w: vW, h: vH });
        S.stats.renderErrors++;
        frame.close();
        return;
    }

    updateSize();

    if (S.W !== vW || S.H !== vH) {
        S.W = vW;
        S.H = vH;
        log.info('RENDER', 'Resolution changed', { w: vW, h: vH });
    }

    const vp = S.lastVp = calcVp(vW, vH, canvasW, canvasH);
    let success = false;

    try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);

        const glError = gl.getError();
        if (glError !== gl.NO_ERROR) {
            logVideoDrop('GL texture upload error', { code: glError, w: vW, h: vH });
            S.stats.renderErrors++;
        } else {
            gl.flush();
            success = true;
            hasValidTexture = true;
            lastViewport = { ...vp };
        }
    } catch (e) {
        logVideoDrop('Texture upload exception', { error: e.message });
        S.stats.renderErrors++;
        if (hasValidTexture && lastViewport) {
            gl.viewport(lastViewport.x, lastViewport.y, lastViewport.w, lastViewport.h);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
    }

    if (success) {
        clearLetterbox(vp);
        gl.viewport(vp.x, vp.y, vp.w, vp.h);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }
    if (meta?.capTs) {
        S.frameMeta.delete(meta.capTs);
    }

    recordRenderTime(performance.now() - startTime);
    frame.close();
};
export const setCursorStyle = type => {
    const cursor = (S.relativeMouseMode || S.pointerLocked) ? '' : (CURSOR_TYPES[type] || 'default');
    canvas.style.cursor = cursor;
    log.debug('RENDER', 'Cursor set', { type, cursor });
};

export const resetCursorStyle = () => {
    canvas.style.cursor = 'default';
};
export const queueFrameForPresentation = entry => {
    const now = performance.now();
    const m = S.jitterMetrics;
    const localAge = now - entry.queuedAt;
    if (localAge > C.JITTER_MAX_AGE_MS) {
        m.framesDroppedLate++;
        logVideoDrop('Frame too old', { ageMs: localAge.toFixed(2) });
        safe(() => entry.frame.close(), undefined, 'RENDER');
        return;
    }
    m.frameAgeSum += localAge;
    m.frameAgeSamples++;
    if (S.clockSync.valid && entry.serverCapTs) {
        const serverAge = serverFrameAgeMs(entry.serverCapTs);
        m.serverAgeSum += serverAge;
        m.serverAgeSamples++;
    }
    if (m.lastPresentTs > 0) {
        m.presentIntervals.push(now - m.lastPresentTs);
        if (m.presentIntervals.length > C.JITTER_SAMPLES) {
            m.presentIntervals.shift();
        }
    }
    m.lastPresentTs = now;

    renderFrame(entry.frame, entry.meta);
};
export const resetRenderer = () => {
    S.jitterMetrics.lastPresentTs = 0;
    hasValidTexture = false;
    lastViewport = null;
    resetCursorStyle();
    log.debug('RENDER', 'Renderer reset');
};
const redraw = () => {
    updateSize();

    if (canvasW > 0 && canvasH > 0 && gl) {
        gl.viewport(0, 0, canvasW, canvasH);
        gl.clear(gl.COLOR_BUFFER_BIT);

        if (S.W > 0 && S.H > 0 && hasValidTexture) {
            const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH);
            clearLetterbox(vp);
            gl.viewport(vp.x, vp.y, vp.w, vp.h);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
    }
};
let resizeTimeout;
const onResize = () => {
    clearTimeout(resizeTimeout);
    resizeTimeout = setTimeout(() => requestAnimationFrame(redraw), 50);
};
window.addEventListener('resize', onResize);
new MutationObserver(() => {
    onResize();
    setTimeout(onResize, 350);
}).observe(document.body, { attributes: true, attributeFilter: ['class'] });
requestAnimationFrame(redraw);
init();
