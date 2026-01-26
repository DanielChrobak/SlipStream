import { C, S, serverFrameAgeMs, recordRenderTime } from './state.js';

// Canvas and WebGL context
export const canvas = document.getElementById('c');
export let canvasW = 0;
export let canvasH = 0;

export const gl = canvas.getContext('webgl2', {
    alpha: false,
    depth: false,
    stencil: false,
    antialias: false,
    desynchronized: true,
    powerPreference: 'high-performance',
    preserveDrawingBuffer: false
});

let lastSuccessfulVp = null;
let hasValidTexture = false;

// Initialize WebGL
const init = () => {
    if (!gl) {
        console.error('WebGL2 not supported');
        return false;
    }

    // Disable unnecessary features
    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.DITHER);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

    // Create shaders
    const vertexSource = `#version 300 es
        in vec2 a_pos, a_tex;
        out vec2 v;
        void main() {
            gl_Position = vec4(a_pos, 0., 1.);
            v = a_tex;
        }`;

    const fragmentSource = `#version 300 es
        precision highp float;
        in vec2 v;
        uniform sampler2D u;
        out vec4 o;
        void main() {
            o = texture(u, v);
        }`;

    const [vs, fs] = [gl.VERTEX_SHADER, gl.FRAGMENT_SHADER].map((type, i) => {
        const shader = gl.createShader(type);
        gl.shaderSource(shader, i === 0 ? vertexSource : fragmentSource);
        gl.compileShader(shader);
        return shader;
    });

    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS) ||
        !gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
        console.error('Shader:', gl.getShaderInfoLog(vs) || gl.getShaderInfoLog(fs));
        return false;
    }

    // Create program
    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        console.error('Link:', gl.getProgramInfoLog(program));
        return false;
    }

    gl.useProgram(program);

    // Set up vertex buffer
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([
            -1, -1, 0, 1,
             1, -1, 1, 1,
            -1,  1, 0, 0,
             1,  1, 1, 0
        ]),
        gl.STATIC_DRAW
    );

    // Set up vertex attributes
    ['a_pos', 'a_tex'].forEach((name, i) => {
        const loc = gl.getAttribLocation(program, name);
        gl.enableVertexAttribArray(loc);
        gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 16, i * 8);
    });

    // Set up texture
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

    updateSize();
    clear();

    return true;
};

// Update canvas size based on device pixel ratio
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

// Calculate viewport for aspect ratio preservation
export const calcVp = (videoW, videoH, displayW, displayH) => {
    const videoAspect = videoW / videoH;
    const displayAspect = displayW / displayH;

    if (displayAspect > videoAspect) {
        // Display is wider than video - letterbox on sides
        const w = Math.round(displayH * videoAspect);
        return {
            x: Math.round((displayW - w) / 2),
            y: 0,
            w: w,
            h: displayH
        };
    } else {
        // Display is taller than video - letterbox on top/bottom
        const h = Math.round(displayW / videoAspect);
        return {
            x: 0,
            y: Math.round((displayH - h) / 2),
            w: displayW,
            h: h
        };
    }
};

// Clear the canvas
const clear = () => {
    gl.viewport(0, 0, canvasW, canvasH);
    gl.clearColor(0.039, 0.039, 0.043, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
};

// Clear letterbox areas
const clearLetterbox = (vp) => {
    gl.enable(gl.SCISSOR_TEST);
    gl.clearColor(0.039, 0.039, 0.043, 1);

    // Left letterbox
    if (vp.x > 0) {
        gl.scissor(0, 0, vp.x, canvasH);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }

    // Right letterbox
    if (vp.x + vp.w < canvasW) {
        gl.scissor(vp.x + vp.w, 0, canvasW - vp.x - vp.w, canvasH);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }

    // Bottom letterbox
    if (vp.y > 0) {
        gl.scissor(0, 0, canvasW, vp.y);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }

    // Top letterbox
    if (vp.y + vp.h < canvasH) {
        gl.scissor(0, vp.y + vp.h, canvasW, canvasH - vp.y - vp.h);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }

    gl.disable(gl.SCISSOR_TEST);
};

// Render a video frame
const renderFrame = (frame, meta) => {
    const renderStart = performance.now();
    const { displayWidth: vW, displayHeight: vH } = frame;

    // Validate frame dimensions
    if (!vW || !vH || vW <= 0 || vH <= 0) {
        S.stats.renderErrors++;
        frame.close();
        return;
    }

    updateSize();

    // Update video dimensions
    if (S.W !== vW || S.H !== vH) {
        S.W = vW;
        S.H = vH;
    }

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

        // Try to render last valid frame
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

    // Clean up frame metadata
    if (meta?.capTs) {
        S.frameMeta.delete(meta.capTs);
    }

    S.stats.rend++;
    recordRenderTime(performance.now() - renderStart);
    frame.close();
};

// Queue a frame for presentation
export const queueFrameForPresentation = entry => {
    const now = performance.now();
    const m = S.jitterMetrics;
    const queue = S.presentQueue;

    // Track queue metrics
    m.queueDepthSum += queue.length;
    m.queueDepthSamples++;
    m.maxQueueDepth = Math.max(m.maxQueueDepth, queue.length);

    const maxQueueSize = C.JITTER_MAX_FRAMES || 2;
    const maxAgeMs = C.JITTER_MAX_AGE_MS || 50;

    // Drop old frames from queue
    while (queue.length > 0) {
        const oldest = queue[0];
        const age = now - oldest.queuedAt;

        if (age > maxAgeMs || queue.length >= maxQueueSize) {
            m.framesDroppedLate++;
            try {
                queue.shift().frame.close();
            } catch {}
        } else {
            break;
        }
    }

    // Drop frames if queue is full
    while (queue.length >= maxQueueSize) {
        m.framesDroppedOverflow++;
        try {
            queue.shift().frame.close();
        } catch {}
    }

    // Track frame age metrics
    const localAge = now - entry.queuedAt;
    m.frameAgeSum += localAge;
    m.frameAgeSamples++;
    m.minFrameAgeMs = Math.min(m.minFrameAgeMs, localAge);
    m.maxFrameAgeMs = Math.max(m.maxFrameAgeMs, localAge);

    // Track server-side frame age
    if (S.clockSync.valid && entry.serverCapTs) {
        const serverAge = serverFrameAgeMs(entry.serverCapTs);
        m.serverAgeSum += serverAge;
        m.serverAgeSamples++;
        m.minServerAgeMs = Math.min(m.minServerAgeMs, serverAge);
        m.maxServerAgeMs = Math.max(m.maxServerAgeMs, serverAge);
    }

    // Track presentation intervals
    if (m.lastPresentTs > 0) {
        m.presentIntervals.push(now - m.lastPresentTs);
        if (m.presentIntervals.length > C.PRESENT_INTERVAL_WINDOW) {
            m.presentIntervals.shift();
        }
    }
    m.lastPresentTs = now;

    // Render immediately
    renderFrame(entry.frame, entry.meta);
};

// Start the presentation loop
export const startPresentLoop = () => {
    if (S.presentLoopRunning) return;

    S.presentLoopRunning = true;
    S.jitterMetrics.lastPresentTs = 0;
};

// Stop the presentation loop
export const stopPresentLoop = () => {
    S.presentLoopRunning = false;

    // Close all queued frames
    S.presentQueue.forEach(entry => {
        try {
            entry.frame.close();
        } catch {}
    });
    S.presentQueue = [];

    hasValidTexture = false;
    lastSuccessfulVp = null;
};

// Handle window resize
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

// Set up resize listeners
window.addEventListener('resize', handleResize);
new MutationObserver(() => setTimeout(handleResize, 100))
    .observe(document.body, { attributes: true, attributeFilter: ['class'] });

// Initialize
init();
