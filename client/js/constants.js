export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920, CODEC_SET: 0x434F4443, CODEC_ACK: 0x434F4441,
    CODEC_CAPS: 0x434F4350, MOUSE_MOVE_REL: 0x4D4F5652, CLIPBOARD_DATA: 0x434C4950,
    CLIPBOARD_GET: 0x434C4754, KICKED: 0x4B49434B, CURSOR_CAPTURE: 0x43555243,
    CURSOR_SHAPE: 0x43555253, AUDIO_ENABLE: 0x41554445, MIC_DATA: 0x4D494344, MIC_ENABLE: 0x4D494345,
    ENCODER_INFO: 0x49434E45, VERSION: 0x56455253
};

export const CURSOR_TYPES = ['default', 'text', 'pointer', 'wait', 'progress', 'crosshair', 'move',
    'ew-resize', 'ns-resize', 'nwse-resize', 'nesw-resize', 'not-allowed', 'help', 'none'];

export const CODECS = {
    AV1: { id: 0, name: 'AV1', codec: 'av01.0.05M.08' },
    H265: { id: 1, name: 'H.265', codec: 'hev1.1.6.L93.B0' },
    H264: { id: 2, name: 'H.264', codec: 'avc1.42001f' }
};

export const CODEC_KEYS = ['av1', 'h265', 'h264'];

export const C = {
    HEADER: 55, AUDIO_HEADER: 24, PING_MS: 200, MAX_FRAMES: 64, FRAME_TIMEOUT_MS: 900,
    KEY_REQ_MIN_INTERVAL_MS: 350, KEY_RETRY_INTERVAL_MS: 700,
    FEC_GROUP_SIZE: 10,
    AUDIO_RATE: 48000, AUDIO_CH: 2,
    MIC_HEADER: 24, MIC_RATE: 48000, MIC_CH: 1, MIC_FRAME_MS: 10,
    DC_CONTROL: { ordered: 1, maxRetransmits: 3 },
    DC_VIDEO: { ordered: 0, maxRetransmits: 0 },
    DC_AUDIO: { ordered: 0, maxRetransmits: 0 },
    DC_INPUT: { ordered: 1, maxRetransmits: 3 },
    DC_MIC: { ordered: 0, maxRetransmits: 0 },
    JITTER_MAX_AGE_MS: 50, JITTER_SAMPLES: 60,
    CLOCK_OFFSET_SAMPLES: 8, METRICS_LOG_INTERVAL_MS: 1000
};
