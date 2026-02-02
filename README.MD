# SlipStream

Low-latency remote desktop streaming over WebRTC with hardware-accelerated video encoding.

## Overview

SlipStream captures the Windows desktop using the Windows Graphics Capture API and encodes frames in real-time using NVIDIA NVENC. Video and audio are streamed to web browsers via WebRTC data channels with DTLS encryption. The client renders frames using WebGL2 and decodes video/audio using the WebCodecs API.

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              SERVER (Windows)                                │
├──────────────────────────────────────────────────────────────────────────────┤
│  Screen Capture          Encoder              WebRTC              Audio      │
│  ┌──────────────┐     ┌──────────────┐    ┌──────────────┐     ┌──────────┐  │
│  │ WGC API      │───▶│ NVENC        │───▶│ Data Channel │◀──▶│ WASAPI   │  │
│  │ D3D11 Fence  │     │ H.264 / AV1  │    │ libdatachan  │     │ Opus     │  │
│  │ 6-tex pool   │     │ FFmpeg       │    │              │     │ 96 kbps  │  │
│  └──────────────┘     └──────────────┘    └──────────────┘     └──────────┘  │
│                                                │                             │
│                            ┌───────────────────┼───────────────────┐         │
│                            │      HTTPS Server (port 443)          │         │
│                            │   cpp-httplib + OpenSSL               │         │
│                            │   JWT Auth + Rate Limiting            │         │
│                            └───────────────────┼───────────────────┘         │
└────────────────────────────────────────────────┼─────────────────────────────┘
                                                 │
                            WebRTC (UDP 50000-50020)
                                                 │
┌────────────────────────────────────────────────┼─────────────────────────────┐
│                              CLIENT (Browser)                                │
├──────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐     ┌──────────────┐    ┌──────────────┐     ┌──────────┐  │
│  │ VideoDecoder │───▶│ WebGL2       │    │ AudioDecoder  │───▶│ Worklet  │  │
│  │ HW or SW     │     │ Letterbox    │    │ Opus         │     │ RingBuf  │  │
│  └──────────────┘     └──────────────┘    └──────────────┘     └──────────┘  │
│                                                                              │
│  Input Handler ──────▶ Mouse/Keyboard ──────▶ Data Channel ──────▶ Server  │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Features

- **Dual Codec Support** - H.264 and AV1 via NVIDIA NVENC
- **Hardware Decoding** - WebCodecs API with automatic HW/SW fallback
- **System Audio** - WASAPI loopback capture with Opus encoding
- **Multi-Monitor** - Live monitor switching with optional tabbed UI
- **Clipboard Sync** - Bidirectional text clipboard (Ctrl+C/V)
- **Relative Mouse** - Pointer lock mode for games and full-screen apps
- **Session Auth** - PBKDF2-SHA256 password hashing with JWT tokens
- **Rate Limiting** - Protection against brute-force attacks
- **Auto SSL** - Self-signed certificate generation on first run
- **Debug Stats** - Real-time performance overlay

## Requirements

### Server

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10/11 64-bit (build 1903+) |
| GPU | NVIDIA GPU with NVENC support |
| IDE | Visual Studio 2022 with C++20 Desktop workload |
| Package Manager | [vcpkg](https://github.com/microsoft/vcpkg) |

### Client

| Component | Requirement |
|-----------|-------------|
| Browser | Chrome 94+, Edge 94+, Firefox 98+ |
| APIs | WebRTC, VideoDecoder, AudioDecoder, WebGL2 |

## Quick Start

### 1. Install vcpkg

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
```

### 2. Build

```batch
build.bat
```

Output: `build\bin\Release\SlipStream.exe`

### 3. Run

```batch
run.bat
```

On first run:
- Prompts for username (3-32 alphanumeric characters)
- Prompts for password (8+ characters with at least one letter and digit)
- Generates self-signed SSL certificate (`server.crt`, `server.key`)
- Saves hashed credentials to `auth.json`

### 4. Connect

Open browser to `https://<HOST_IP>:443` and log in with your credentials.

> **Note:** Self-signed certificate will trigger a browser warning. Click through to proceed.

## Network Configuration

| Port | Protocol | Purpose |
|------|----------|---------|
| 443 | TCP | HTTPS server and WebRTC signaling |
| 50000-50020 | UDP | WebRTC media transport |

### Firewall

```powershell
netsh advfirewall firewall add rule name="SlipStream HTTPS" dir=in action=allow protocol=tcp localport=443
netsh advfirewall firewall add rule name="SlipStream WebRTC" dir=in action=allow protocol=udp localport=50000-50020
```

## Security

### Password Storage

| Parameter | Value |
|-----------|-------|
| Algorithm | PBKDF2-HMAC-SHA256 |
| Iterations | 600,000 |
| Salt | 16 bytes (random) |
| Key Length | 32 bytes |

Passwords are never stored in plain text. Only the salt and derived hash are saved in `auth.json`.

### Rate Limiting

| Threshold | Action |
|-----------|--------|
| 5 failed attempts in 15 min | 30-minute IP lockout |
| Successful login | Clears attempt counter |

### JWT Sessions

| Parameter | Value |
|-----------|-------|
| Algorithm | HS256 |
| Issuer | slipstream |
| Expiry | 24 hours |
| Storage | HttpOnly cookie |

### SSL Certificates

Auto-generated on first run:
- 2048-bit RSA key
- 10-year validity
- Subject Alt Names: localhost, 127.0.0.1, 0.0.0.0

To use custom certificates, replace `server.crt` and `server.key` before starting.

## API Endpoints

| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/` | GET | No | Web client HTML |
| `/styles.css` | GET | No | Stylesheet |
| `/js/*.js` | GET | No | JavaScript modules |
| `/api/auth` | POST | No | Login with `{username, password}` |
| `/api/session` | GET | Cookie | Validate current session |
| `/api/logout` | POST | No | Clear session cookie |
| `/api/offer` | POST | Cookie | WebRTC SDP offer exchange |

## Video Pipeline

### Capture

| Component | Detail |
|-----------|--------|
| API | Windows Graphics Capture |
| Texture Pool | 6 textures with D3D11 fence sync |
| Frame Buffer | 4-frame ring buffer |
| Cursor | Captured in stream |
| Border | Disabled (if supported) |

### Encoding

| Setting | H.264 | AV1 |
|---------|-------|-----|
| Encoder | h264_nvenc | av1_nvenc |
| Preset | P1 (fastest) | P1 (fastest) |
| Tune | Ultra-low latency | Ultra-low latency |
| Rate Control | VBR | VBR |
| CQ Level | 23 | 28 |
| Keyframe Interval | 2 seconds | 2 seconds |
| B-Frames | 0 | 0 |
| Color Space | BT.709 | BT.709 |
| Color Range | Full (JPEG) | Full (PC) |

**Bitrate Formula:** `0.18085 × width × height × fps` bps

Example: 1920×1080 @ 60fps = ~22.5 Mbps

### Transport

| Parameter | Value |
|-----------|-------|
| Chunk Size | 1400 bytes max |
| Header Size | 21 bytes |
| Video Buffer | 256 KB threshold |
| Max Queue | 3 frames worth |
| Delivery | Unreliable, unordered |

### Video Packet Header (21 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | timestamp | Capture timestamp (microseconds) |
| 8 | 4 | encodeTimeUs | Encode duration |
| 12 | 4 | frameId | Frame sequence number |
| 16 | 2 | chunkIndex | Current chunk index |
| 18 | 2 | totalChunks | Total chunks in frame |
| 20 | 1 | frameType | 1=keyframe, 0=delta |

## Audio Pipeline

### Capture

| Parameter | Value |
|-----------|-------|
| API | WASAPI Loopback |
| Mode | Shared |
| Sample Rate | 48,000 Hz (resampled if needed) |
| Channels | Stereo (max 2) |
| Frame Duration | 10 ms (480 samples) |

### Encoding

| Parameter | Value |
|-----------|-------|
| Codec | Opus |
| Application | Restricted Low Delay |
| Bitrate | 96 kbps |
| Complexity | 3 |
| Signal Type | Music |

### Transport

| Parameter | Value |
|-----------|-------|
| Audio Buffer | 128 KB threshold |
| Max Queue | 4 packets |
| Delivery | Unreliable, unordered |

### Audio Packet Header (16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | 0x41554449 ("AUDI") |
| 4 | 8 | timestamp | Capture timestamp |
| 12 | 2 | samples | Sample count |
| 14 | 2 | dataLength | Opus payload size |

### Client Playback

| Component | Detail |
|-----------|--------|
| Decoder | AudioDecoder (WebCodecs) |
| Processor | AudioWorklet |
| Buffer | Ring buffer (2400 samples) |
| Target Latency | 480 samples (~10ms) |
| Max Buffer | 1200 samples (~25ms) |

## Input Protocol

### Mouse Move Absolute (12 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x4D4F5645) |
| 4 | 4 | x (float 0-1) |
| 8 | 4 | y (float 0-1) |

### Mouse Move Relative (8 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x4D4F5652) |
| 4 | 2 | dx (int16) |
| 6 | 2 | dy (int16) |

### Mouse Button (6 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x4D42544E) |
| 4 | 1 | button (0-4) |
| 5 | 1 | action (1=down, 0=up) |

Button mapping: 0=left, 1=right, 2=middle, 3=X1, 4=X2

### Mouse Wheel (8 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x4D57484C) |
| 4 | 2 | deltaX (int16) |
| 6 | 2 | deltaY (int16) |

### Keyboard (10 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x4B455920) |
| 4 | 2 | keyCode (JS keyCode) |
| 6 | 2 | scanCode |
| 8 | 1 | action (1=down, 0=up) |
| 9 | 1 | modifiers |

Modifiers: Ctrl=1, Alt=2, Shift=4, Meta=8

### Clipboard Data (8 + N bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x434C4950) |
| 4 | 4 | length |
| 8 | N | UTF-8 text (max 1MB) |

### Rate Limits

| Type | Max per Second |
|------|----------------|
| Mouse moves | 500 |
| Clicks | 50 |
| Keystrokes | 100 |

### Blocked Inputs

- Windows key (left/right)
- Ctrl+Alt+Delete

## WebRTC Data Channels

| Channel | Ordered | MaxRetransmits | Purpose |
|---------|---------|----------------|---------|
| control | Yes | 3 | Commands, ping, monitor list |
| video | No | 0 | Video frames |
| audio | No | 0 | Audio packets |
| input | Yes | 3 | Mouse/keyboard events |

## Client Modules

| File | Purpose |
|------|---------|
| `state.js` | Shared state, constants, metrics, clock sync |
| `network.js` | HTTP auth, WebRTC signaling, data channels |
| `renderer.js` | WebGL2 rendering with aspect ratio letterboxing |
| `media.js` | VideoDecoder, AudioDecoder, AudioWorklet |
| `input.js` | Mouse/keyboard capture, batching, pointer lock |
| `ui.js` | Settings panel, fullscreen, tabbed mode, stats |

### Clock Synchronization

- Ping interval: 200ms
- Sample count: 8
- Offset calculation: Median filter
- RTT estimation: Median of samples

### Frame Handling

| Parameter | Value |
|-----------|-------|
| Max buffered frames | 8 |
| Frame timeout | 200ms |
| Max frame age | 50ms |
| Decode queue limit | 6 |

## UI Features

### Settings Panel

Access by clicking the right edge of the screen:

- **Logout** - Disconnect and clear session
- **Fullscreen** - Enter fullscreen with keyboard lock (Escape captured)
- **Audio** - Toggle system audio playback
- **Monitor** - Switch between displays
- **Frame Rate** - 15/30/60/120/144 or custom (1-240)
- **Codec** - H.264 or AV1 (shows HW/SW status)
- **Tabbed Mode** - Monitor tabs at top of screen
- **Debug Stats** - Real-time performance overlay
- **Relative Mouse** - Pointer lock mode for gaming
- **Clipboard Sync** - Enable Ctrl+C/V synchronization

### Tabbed Mode

When enabled, displays a tab strip showing all monitors with:
- Monitor name (friendly name from EDID)
- Resolution
- Primary indicator (star icon)

### Debug Stats Overlay

Real-time metrics display:

| Section | Metrics |
|---------|---------|
| Throughput | FPS (actual/target), bitrate, resolution, codec |
| Latency | RTT, frame age |
| Jitter | Frame interval mean, std dev |
| Decode | Time, queue size, HW acceleration status |
| Render | Time, frame count |
| Network | Packet count, average size |
| Drops | Dropped, timeout, late, decode errors |
| Audio | Packets, decoded, buffer health, underruns, overflows |
| Input | Mouse moves, clicks, keystrokes |
| Session | Uptime, total frames, total data |

## Server Threads

| Thread | Priority | Purpose |
|--------|----------|---------|
| Main | Above Normal | HTTPS server |
| Encoder | Time Critical | Frame encoding loop |
| Audio | Highest | Audio capture and encoding |

## Dependencies

Managed via vcpkg (`vcpkg.json`):

| Package | Purpose |
|---------|---------|
| libdatachannel | WebRTC implementation |
| cpp-httplib[openssl] | HTTPS server |
| nlohmann-json | JSON parsing |
| opus | Audio encoding |
| openssl | Cryptography, SSL/TLS |
| ffmpeg[nvcodec,avcodec] | Video encoding |
| jwt-cpp | JWT token handling |

## Building

### Standard Build

```batch
build.bat
```

### Installer Build

```batch
build_installer.bat
```

Creates `SlipStream-1.0.0-win64.exe` (NSIS) or `.zip` (fallback).

## Troubleshooting

| Problem | Solution |
|---------|----------|
| vcpkg not found | Set `VCPKG_ROOT` or install to `C:\vcpkg` |
| NVENC unavailable | Requires NVIDIA GPU with NVENC support |
| Connection refused | Check firewall for TCP 443, UDP 50000-50020 |
| Black screen | Wait for keyframe or click to focus |
| No audio | Click "Enable" in settings panel |
| Input not working | Click the video canvas to capture focus |
| Certificate warning | Expected with self-signed cert |

## Known Limitations

- Windows server only
- Single client connection (new client kicks existing)
- NVIDIA GPU required for encoding
- No file transfer

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial use requires separate license.

See LICENSE.txt for full terms.

## Copyright

(c) 2025-2026 Daniel Chrobak. All rights reserved.


