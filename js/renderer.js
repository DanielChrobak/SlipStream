import { C, S, serverFrameAgeMs, recordRenderTime } from './state.js';

export const canvas = document.getElementById('c');
export let canvasW = 0, canvasH = 0;

export const gl = canvas.getContext('webgl2', {
    alpha: false, depth: false, stencil: false, antialias: false,
    desynchronized: true, powerPreference: 'high-performance', preserveDrawingBuffer: false
});

let lastSuccessfulVp = null, hasValidTexture = false;

const init = () => {
    if (!gl) return false;
    gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST); gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    const vs = gl.createShader(gl.VERTEX_SHADER), fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(vs, `#version 300 es
        in vec2 a_pos, a_tex; out vec2 v;
        void main() { gl_Position = vec4(a_pos, 0., 1.); v = a_tex; }`);
    gl.shaderSource(fs, `#version 300 es
        precision highp float; in vec2 v; uniform sampler2D u; out vec4 o;
        void main() { o = texture(u, v); }`);
    gl.compileShader(vs); gl.compileShader(fs);

    const prog = gl.createProgram();
    gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) return false;

    gl.useProgram(prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0]), gl.STATIC_DRAW);

    [['a_pos', 0], ['a_tex', 8]].forEach(([n, o]) => {
        const loc = gl.getAttribLocation(prog, n);
        gl.enableVertexAttribArray(loc);
        gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 16, o);
    });

    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T].forEach(p => gl.texParameteri(gl.TEXTURE_2D, p, gl.CLAMP_TO_EDGE));
    [gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER].forEach(p => gl.texParameteri(gl.TEXTURE_2D, p, gl.LINEAR));

    updateSize();
    gl.viewport(0, 0, canvasW, canvasH);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    return true;
};

export const updateSize = () => {
    const dpr = devicePixelRatio || 1;
    const dW = Math.round(canvas.clientWidth * dpr), dH = Math.round(canvas.clientHeight * dpr);
    if (canvas.width !== dW || canvas.height !== dH) { canvas.width = canvasW = dW; canvas.height = canvasH = dH; }
};

export const calcVp = (vW, vH, dW, dH) => {
    const vA = vW / vH, dA = dW / dH;
    if (dA > vA) { const w = Math.round(dH * vA); return { x: Math.round((dW - w) / 2), y: 0, w, h: dH }; }
    const h = Math.round(dW / vA); return { x: 0, y: Math.round((dH - h) / 2), w: dW, h };
};

const clearLetterbox = vp => {
    gl.enable(gl.SCISSOR_TEST);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    [[0, 0, vp.x, canvasH], [vp.x + vp.w, 0, canvasW - vp.x - vp.w, canvasH], [0, 0, canvasW, vp.y], [0, vp.y + vp.h, canvasW, canvasH - vp.y - vp.h]]
        .filter(([,,w,h]) => w > 0 && h > 0).forEach(r => { gl.scissor(...r); gl.clear(gl.COLOR_BUFFER_BIT); });
    gl.disable(gl.SCISSOR_TEST);
};

const renderFrame = (frame, meta) => {
    const renderStart = performance.now();
    const { displayWidth: vW, displayHeight: vH } = frame;
    if (!vW || !vH || vW <= 0 || vH <= 0) { S.stats.renderErrors++; frame.close(); return; }

    updateSize();
    if (S.W !== vW || S.H !== vH) { S.W = vW; S.H = vH; }

    const vp = S.lastVp = calcVp(vW, vH, canvasW, canvasH);
    let ok = false;

    try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
        gl.flush();
        ok = hasValidTexture = true;
        lastSuccessfulVp = { ...vp };
    } catch {
        S.stats.renderErrors++;
        if (hasValidTexture && lastSuccessfulVp) {
            gl.viewport(lastSuccessfulVp.x, lastSuccessfulVp.y, lastSuccessfulVp.w, lastSuccessfulVp.h);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
    }

    if (ok) { clearLetterbox(vp); gl.viewport(vp.x, vp.y, vp.w, vp.h); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }
    if (meta?.capTs) S.frameMeta.delete(meta.capTs);
    S.stats.rend++;
    recordRenderTime(performance.now() - renderStart);
    frame.close();
};

export const queueFrameForPresentation = entry => {
    const now = performance.now(), m = S.jitterMetrics, q = S.presentQueue;
    const maxQ = C.JITTER_MAX_FRAMES || 2, maxAge = C.JITTER_MAX_AGE_MS || 50;

    m.queueDepthSum += q.length; m.queueDepthSamples++; m.maxQueueDepth = Math.max(m.maxQueueDepth, q.length);

    while (q.length > 0 && (now - q[0].queuedAt > maxAge || q.length >= maxQ)) {
        m[now - q[0].queuedAt > maxAge ? 'framesDroppedLate' : 'framesDroppedOverflow']++;
        try { q.shift().frame.close(); } catch {}
    }

    const localAge = now - entry.queuedAt;
    m.frameAgeSum += localAge; m.frameAgeSamples++;
    m.minFrameAgeMs = Math.min(m.minFrameAgeMs, localAge);
    m.maxFrameAgeMs = Math.max(m.maxFrameAgeMs, localAge);

    if (S.clockSync.valid && entry.serverCapTs) {
        const serverAge = serverFrameAgeMs(entry.serverCapTs);
        m.serverAgeSum += serverAge; m.serverAgeSamples++;
        m.minServerAgeMs = Math.min(m.minServerAgeMs, serverAge);
        m.maxServerAgeMs = Math.max(m.maxServerAgeMs, serverAge);
    }

    if (m.lastPresentTs > 0) {
        m.presentIntervals.push(now - m.lastPresentTs);
        if (m.presentIntervals.length > C.PRESENT_INTERVAL_WINDOW) m.presentIntervals.shift();
    }
    m.lastPresentTs = now;
    renderFrame(entry.frame, entry.meta);
};

export const startPresentLoop = () => {
    if (S.presentLoopRunning) return;
    S.presentLoopRunning = true;
    S.jitterMetrics.lastPresentTs = 0;
};

export const stopPresentLoop = () => {
    S.presentLoopRunning = false;
    S.presentQueue.forEach(e => { try { e.frame.close(); } catch {} });
    S.presentQueue = [];
    hasValidTexture = false;
    lastSuccessfulVp = null;
};

let resizeTimeout;
const handleResize = () => {
    clearTimeout(resizeTimeout);
    resizeTimeout = setTimeout(() => {
        updateSize();
        if (S.W > 0 && S.H > 0 && gl && hasValidTexture) {
            const vp = calcVp(S.W, S.H, canvasW, canvasH);
            clearLetterbox(vp);
            gl.viewport(vp.x, vp.y, vp.w, vp.h);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
    }, 50);
};

window.addEventListener('resize', handleResize);
new MutationObserver(() => setTimeout(handleResize, 100)).observe(document.body, { attributes: true, attributeFilter: ['class'] });
init();
