import { C, S, serverFrameAgeMs, recordRenderTime } from './state.js';

export const canvas = document.getElementById('c');
export let canvasW = 0;
export let canvasH = 0;

export const gl = canvas.getContext('webgl2', {
    alpha: false, depth: false, stencil: false, antialias: false,
    desynchronized: true, powerPreference: 'high-performance', preserveDrawingBuffer: false
});

let lastSuccessfulVp = null;
let hasValidTexture = false;

const init = () => {
    if (!gl) { console.error('WebGL2 not supported'); return false; }

    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    const vertexSource = `#version 300 es
        in vec2 a_pos, a_tex;
        out vec2 v;
        void main() { gl_Position = vec4(a_pos, 0., 1.); v = a_tex; }`;

    const fragmentSource = `#version 300 es
        precision highp float;
        in vec2 v;
        uniform sampler2D u;
        out vec4 o;
        void main() { o = texture(u, v); }`;

    const [vs, fs] = [gl.VERTEX_SHADER, gl.FRAGMENT_SHADER].map((type, i) => {
        const shader = gl.createShader(type);
        gl.shaderSource(shader, i === 0 ? vertexSource : fragmentSource);
        gl.compileShader(shader);
        return shader;
    });

    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS) || !gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
        console.error('Shader:', gl.getShaderInfoLog(vs) || gl.getShaderInfoLog(fs));
        return false;
    }

    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        console.error('Link:', gl.getProgramInfoLog(program));
        return false;
    }

    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0]), gl.STATIC_DRAW);

    ['a_pos', 'a_tex'].forEach((name, i) => {
        const loc = gl.getAttribLocation(program, name);
        gl.enableVertexAttribArray(loc);
        gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 16, i * 8);
    });

    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

    updateSize();
    clear();
    return true;
};

export const updateSize = () => {
    const dpr = devicePixelRatio || 1;
    const displayW = Math.round(canvas.clientWidth * dpr);
    const displayH = Math.round(canvas.clientHeight * dpr);
    if (canvas.width !== displayW || canvas.height !== displayH) {
        canvas.width = displayW;
        canvas.height = displayH;
        canvasW = displayW;
        canvasH = displayH;
    }
};

export const calcVp = (videoW, videoH, displayW, displayH) => {
    const videoAspect = videoW / videoH;
    const displayAspect = displayW / displayH;
    if (displayAspect > videoAspect) {
        const w = Math.round(displayH * videoAspect);
        return { x: Math.round((displayW - w) / 2), y: 0, w, h: displayH };
    }
    const h = Math.round(displayW / videoAspect);
    return { x: 0, y: Math.round((displayH - h) / 2), w: displayW, h };
};

const clear = () => {
    gl.viewport(0, 0, canvasW, canvasH);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
};

const clearLetterbox = vp => {
    gl.enable(gl.SCISSOR_TEST);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    if (vp.x > 0) { gl.scissor(0, 0, vp.x, canvasH); gl.clear(gl.COLOR_BUFFER_BIT); }
    if (vp.x + vp.w < canvasW) { gl.scissor(vp.x + vp.w, 0, canvasW - vp.x - vp.w, canvasH); gl.clear(gl.COLOR_BUFFER_BIT); }
    if (vp.y > 0) { gl.scissor(0, 0, canvasW, vp.y); gl.clear(gl.COLOR_BUFFER_BIT); }
    if (vp.y + vp.h < canvasH) { gl.scissor(0, vp.y + vp.h, canvasW, canvasH - vp.y - vp.h); gl.clear(gl.COLOR_BUFFER_BIT); }
    gl.disable(gl.SCISSOR_TEST);
};

const renderFrame = (frame, meta) => {
    const renderStart = performance.now();
    const { displayWidth: vW, displayHeight: vH } = frame;

    if (!vW || !vH || vW <= 0 || vH <= 0) {
        S.stats.renderErrors++;
        frame.close();
        return;
    }

    updateSize();
    if (S.W !== vW || S.H !== vH) { S.W = vW; S.H = vH; }

    const vp = S.lastVp = calcVp(vW, vH, canvasW, canvasH);
    let uploadSuccess = false;

    try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
        gl.flush();
        uploadSuccess = true;
        hasValidTexture = true;
        lastSuccessfulVp = { ...vp };
    } catch {
        S.stats.renderErrors++;
        if (hasValidTexture && lastSuccessfulVp) {
            gl.viewport(lastSuccessfulVp.x, lastSuccessfulVp.y, lastSuccessfulVp.w, lastSuccessfulVp.h);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
    }

    if (uploadSuccess) {
        clearLetterbox(vp);
        gl.viewport(vp.x, vp.y, vp.w, vp.h);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }

    if (meta?.capTs) S.frameMeta.delete(meta.capTs);
    S.stats.rend++;
    recordRenderTime(performance.now() - renderStart);
    frame.close();
};

export const queueFrameForPresentation = entry => {
    const now = performance.now();
    const m = S.jitterMetrics;
    const queue = S.presentQueue;

    m.queueDepthSum += queue.length;
    m.queueDepthSamples++;
    m.maxQueueDepth = Math.max(m.maxQueueDepth, queue.length);

    const maxQueueSize = C.JITTER_MAX_FRAMES || 2;
    const maxAgeMs = C.JITTER_MAX_AGE_MS || 50;

    while (queue.length > 0) {
        const oldest = queue[0];
        const age = now - oldest.queuedAt;
        if (age > maxAgeMs || queue.length >= maxQueueSize) {
            m.framesDroppedLate++;
            try { queue.shift().frame.close(); } catch {}
        } else break;
    }

    while (queue.length >= maxQueueSize) {
        m.framesDroppedOverflow++;
        try { queue.shift().frame.close(); } catch {}
    }

    const localAge = now - entry.queuedAt;
    m.frameAgeSum += localAge;
    m.frameAgeSamples++;
    m.minFrameAgeMs = Math.min(m.minFrameAgeMs, localAge);
    m.maxFrameAgeMs = Math.max(m.maxFrameAgeMs, localAge);

    if (S.clockSync.valid && entry.serverCapTs) {
        const serverAge = serverFrameAgeMs(entry.serverCapTs);
        m.serverAgeSum += serverAge;
        m.serverAgeSamples++;
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
    S.presentQueue.forEach(entry => { try { entry.frame.close(); } catch {} });
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
