import { C, S, CURSOR_TYPES, serverFrameAgeMs, recordRenderTime, logVideoDrop, safe } from './state.js';

export const canvas = document.getElementById('c');
export let canvasW = 0, canvasH = 0;

const gl = canvas.getContext('webgl2', { alpha: 0, depth: 0, stencil: 0, antialias: 0, desynchronized: 1, powerPreference: 'high-performance', preserveDrawingBuffer: 0 });
let lastVp = null, hasValidTex = 0;

const updSize = () => {
    const dpr = devicePixelRatio || 1, rect = canvas.getBoundingClientRect(), dW = Math.round(rect.width * dpr), dH = Math.round(rect.height * dpr);
    if (dW > 0 && dH > 0 && (canvas.width !== dW || canvas.height !== dH)) { canvas.width = canvasW = dW; canvas.height = canvasH = dH; return 1; }
    return 0;
};

const init = () => {
    if (!gl) { logVideoDrop('WebGL2 failed', { canvas: !!canvas }); return 0; }
    gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST); gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const vs = gl.createShader(gl.VERTEX_SHADER), fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(vs, `#version 300 es\nin vec2 a_pos,a_tex;out vec2 v;void main(){gl_Position=vec4(a_pos,0.,1.);v=a_tex;}`);
    gl.shaderSource(fs, `#version 300 es\nprecision highp float;in vec2 v;uniform sampler2D u;out vec4 o;void main(){o=texture(u,v);}`);
    gl.compileShader(vs); gl.compileShader(fs);
    const prog = gl.createProgram();
    gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) { logVideoDrop('Shader fail', { vs: gl.getShaderInfoLog(vs), fs: gl.getShaderInfoLog(fs) }); return 0; }
    gl.useProgram(prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1,0,1,1,-1,1,1,-1,1,0,0,1,1,1,0]), gl.STATIC_DRAW);
    [['a_pos', 0], ['a_tex', 8]].forEach(([n, o]) => { const l = gl.getAttribLocation(prog, n); gl.enableVertexAttribArray(l); gl.vertexAttribPointer(l, 2, gl.FLOAT, 0, 16, o); });
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    [gl.TEXTURE_WRAP_S, gl.TEXTURE_WRAP_T, gl.TEXTURE_MIN_FILTER, gl.TEXTURE_MAG_FILTER].forEach((p, i) => gl.texParameteri(gl.TEXTURE_2D, p, i < 2 ? gl.CLAMP_TO_EDGE : gl.LINEAR));
    updSize(); gl.viewport(0, 0, canvasW, canvasH); gl.clearColor(.039, .039, .043, 1); gl.clear(gl.COLOR_BUFFER_BIT);
    return 1;
};

export const calcVp = (vW, vH, dW, dH) => {
    if (dW <= 0 || dH <= 0 || vW <= 0 || vH <= 0) return { x: 0, y: 0, w: dW || 1, h: dH || 1 };
    const vA = vW / vH, dA = dW / dH;
    if (dA > vA) { const w = Math.round(dH * vA); return { x: Math.round((dW - w) / 2), y: 0, w, h: dH }; }
    const h = Math.round(dW / vA); return { x: 0, y: Math.round((dH - h) / 2), w: dW, h };
};

const clearLB = vp => {
    gl.enable(gl.SCISSOR_TEST); gl.clearColor(.039, .039, .043, 1);
    [[0, 0, vp.x, canvasH], [vp.x + vp.w, 0, canvasW - vp.x - vp.w, canvasH], [0, 0, canvasW, vp.y], [0, vp.y + vp.h, canvasW, canvasH - vp.y - vp.h]]
        .filter(([,,w,h]) => w > 0 && h > 0).forEach(r => { gl.scissor(...r); gl.clear(gl.COLOR_BUFFER_BIT); });
    gl.disable(gl.SCISSOR_TEST);
};

const renderFr = (fr, meta) => {
    const st = performance.now(), { displayWidth: vW, displayHeight: vH } = fr;
    if (!vW || !vH || vW <= 0 || vH <= 0) { logVideoDrop('Bad dims', { w: vW, h: vH }); S.stats.renderErrors++; fr.close(); return; }
    updSize();
    if (S.W !== vW || S.H !== vH) { S.W = vW; S.H = vH; }
    const vp = S.lastVp = calcVp(vW, vH, canvasW, canvasH);
    let ok = 0;
    try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, fr);
        if (gl.getError() !== gl.NO_ERROR) { logVideoDrop('GL tex err', { w: vW, h: vH }); S.stats.renderErrors++; }
        else { gl.flush(); ok = hasValidTex = 1; lastVp = { ...vp }; }
    } catch (e) {
        logVideoDrop('Tex upload', { e: e.message });
        S.stats.renderErrors++;
        if (hasValidTex && lastVp) { gl.viewport(lastVp.x, lastVp.y, lastVp.w, lastVp.h); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }
    }
    if (ok) { clearLB(vp); gl.viewport(vp.x, vp.y, vp.w, vp.h); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }
    if (meta?.capTs) S.frameMeta.delete(meta.capTs);
    recordRenderTime(performance.now() - st);
    fr.close();
};

export const setCursorStyle = t => { canvas.style.cursor = (S.relativeMouseMode || S.pointerLocked) ? '' : (CURSOR_TYPES[t] || 'default'); };
export const resetCursorStyle = () => { canvas.style.cursor = 'default'; };

export const queueFrameForPresentation = e => {
    const now = performance.now(), m = S.jitterMetrics, la = now - e.queuedAt;
    if (la > C.JITTER_MAX_AGE_MS) { m.framesDroppedLate++; logVideoDrop('Frame old', { ms: la.toFixed(2) }); safe(() => e.frame.close()); return; }
    m.frameAgeSum += la; m.frameAgeSamples++;
    if (S.clockSync.valid && e.serverCapTs) { const sa = serverFrameAgeMs(e.serverCapTs); m.serverAgeSum += sa; m.serverAgeSamples++; }
    if (m.lastPresentTs > 0) { m.presentIntervals.push(now - m.lastPresentTs); if (m.presentIntervals.length > C.JITTER_SAMPLES) m.presentIntervals.shift(); }
    m.lastPresentTs = now;
    renderFr(e.frame, e.meta);
};

export const resetRenderer = () => { S.jitterMetrics.lastPresentTs = 0; hasValidTex = 0; lastVp = null; resetCursorStyle(); };

let resizeTO;
const onResize = () => {
    clearTimeout(resizeTO);
    resizeTO = setTimeout(() => requestAnimationFrame(() => {
        updSize();
        if (canvasW > 0 && canvasH > 0 && gl) {
            gl.viewport(0, 0, canvasW, canvasH); gl.clearColor(.039, .039, .043, 1); gl.clear(gl.COLOR_BUFFER_BIT);
            if (S.W > 0 && S.H > 0 && hasValidTex) { const vp = S.lastVp = calcVp(S.W, S.H, canvasW, canvasH); clearLB(vp); gl.viewport(vp.x, vp.y, vp.w, vp.h); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }
        }
    }), 50);
};

window.addEventListener('resize', onResize);
new MutationObserver(() => { onResize(); setTimeout(onResize, 350); }).observe(document.body, { attributes: 1, attributeFilter: ['class'] });
requestAnimationFrame(() => { updSize(); if (gl) { gl.viewport(0, 0, canvasW, canvasH); gl.clearColor(.039, .039, .043, 1); gl.clear(gl.COLOR_BUFFER_BIT); } });
init();
