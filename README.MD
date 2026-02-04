# SlipStream

Low-latency remote desktop streaming over WebRTC with NVIDIA hardware-accelerated encoding.

## Overview

SlipStream captures the Windows desktop using the Windows Graphics Capture API and encodes frames in real-time using NVIDIA NVENC (H.264, H.265, or AV1). Video and audio are streamed to web browsers via WebRTC data channels with DTLS encryption. The browser client renders frames using WebGL2 and decodes video/audio using the WebCodecs API.

## Architecture

```
+------------------------------------------------------------------------------+
|                              SERVER (Windows)                                |
+------------------------------------------------------------------------------+
|  Screen Capture         Encoder             WebRTC              Audio        |
|  +--------------+    +--------------+    +--------------+    +----------+    |
|  | WGC API      |--->| NVENC        |--->| Data Channel |<-->| WASAPI   |    |
|  | D3D11 Fence  |    | AV1/H265/264 |    | libdatachan  |    | Opus     |    |
|  | 6-tex pool   |    | FFmpeg       |    |              |    | 96 kbps  |    |
|  +--------------+    +--------------+    +--------------+    +----------+    |
|                                                 |                            |
|                          +----------------------+----------------------+     |
|                          |       HTTPS Server (port 443)               |     |
|                          |    cpp-httplib + OpenSSL                    |     |
|                          |    JWT Auth + Rate Limiting                 |     |
|                          +----------------------+----------------------+     |
+---------------------------------------------+--------------------------------+
                                              |
                          WebRTC (UDP 50000-50020)
                                              |
+---------------------------------------------+--------------------------------+
|                              CLIENT (Browser)                                |
+------------------------------------------------------------------------------+
|  +--------------+    +--------------+    +--------------+    +----------+    |
|  | VideoDecoder |--->| WebGL2       |    | AudioDecoder |--->| Worklet  |    |
|  | HW or SW     |    | Letterbox    |    | Opus         |    | RingBuf  |    |
|  +--------------+    +--------------+    +--------------+    +----------+    |
|                                                                              |
|  Input Handler -----> Mouse/Keyboard -----> Data Channel -----> Server       |
+------------------------------------------------------------------------------+
```

## Features

- **Triple Codec Support** - AV1, H.265, and H.264 via NVIDIA NVENC
- **Hardware Decoding** - WebCodecs API with automatic HW/SW fallback
- **System Audio** - WASAPI loopback capture with Opus encoding
- **Multi-Monitor** - Live monitor switching with tabbed UI option
- **Clipboard Sync** - Bidirectional text clipboard (Ctrl+C/V)
- **Relative Mouse** - Pointer lock mode for games and full-screen apps
- **Cursor Sync** - Remote cursor shape updates (13 standard cursor types)
- **Session Auth** - PBKDF2-SHA256 password hashing with JWT tokens
- **Rate Limiting** - Brute-force protection with IP lockout
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
| APIs | WebRTC, WebCodecs (VideoDecoder, AudioDecoder), WebGL2, AudioWorklet |

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
- Prompts for username (3-32 alphanumeric characters, underscores, hyphens)
- Prompts for password (8+ characters with at least one letter and one digit)
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
| Storage | HttpOnly, Secure, SameSite=Strict cookie |

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
| API | Windows Graphics Capture (WGC) |
| Texture Pool | 6 textures with D3D11 fence sync |
| Frame Buffer | 4-frame ring buffer |
| Cursor | Optional capture in stream |
| Border | Disabled (if OS supports) |

### Encoding

| Setting | H.264 | H.265 | AV1 |
|---------|-------|-------|-----|
| Encoder | h264_nvenc | hevc_nvenc | av1_nvenc |
| Preset | P1 (fastest) | P1 (fastest) | P1 (fastest) |
| Tune | Ultra-low latency | Ultra-low latency | Ultra-low latency |
| Rate Control | VBR | VBR | VBR |
| CQ Level | 23 | 25 | 28 |
| Keyframe Interval | 2 seconds | 2 seconds | 2 seconds |
| B-Frames | 0 | 0 | 0 |
| Color Space | BT.709 | BT.709 | BT.709 |
| Color Range | Full (JPEG) | Full (JPEG) | Full (PC) |

**Bitrate Formula:** `0.18085 × width × height × fps` bps

Example: 1920×1080 @ 60fps ≈ 22.5 Mbps

### Transport

| Parameter | Value |
|-----------|-------|
| Chunk Size | 1400 bytes max (1379 payload + 21 header) |
| Header Size | 21 bytes |
| Video Buffer | 256 KB threshold |
| Max Queue | 3 frames worth of chunks |
| Delivery | Unreliable, unordered (UDP semantics) |

### Video Packet Header (21 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | timestamp | Capture timestamp (microseconds) |
| 8 | 4 | encodeTimeUs | Encode duration (microseconds) |
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
| Sample Rate | 48,000 Hz (resampled if system differs) |
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
| FEC | Disabled |
| DTX | Disabled |

### Transport

| Parameter | Value |
|-----------|-------|
| Audio Buffer | 128 KB threshold |
| Max Queue | 8 packets |
| Delivery | Unreliable, unordered |

### Audio Packet Header (16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | 0x41554449 ("AUDI") |
| 4 | 8 | timestamp | Capture timestamp |
| 12 | 2 | samples | Sample count (480) |
| 14 | 2 | dataLength | Opus payload size |

### Client Playback

| Component | Detail |
|-----------|--------|
| Decoder | AudioDecoder (WebCodecs) |
| Processor | AudioWorklet with ring buffer |
| Buffer Capacity | 9600 samples (200ms) |
| Prebuffer Threshold | 2400 samples (50ms) |
| Target Buffer | 3360 samples (70ms) |
| Max Buffer | 6720 samples (140ms) |

## Input Protocol

### Mouse Move Absolute (12 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x4D4F5645) |
| 4 | 4 | x (float 0.0-1.0) |
| 8 | 4 | y (float 0.0-1.0) |

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

### Cursor Shape (5 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (0x43555253) |
| 4 | 1 | cursorType (0-13, 255) |

Cursor types: 0=default, 1=text, 2=pointer, 3=wait, 4=progress, 5=crosshair, 6=move, 7=ew-resize, 8=ns-resize, 9=nwse-resize, 10=nesw-resize, 11=not-allowed, 12=help, 13=none, 255=custom

### Rate Limits (Server-Enforced)

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
| control | Yes | 3 | Commands, ping/pong, monitor list |
| video | No | 0 | Video frame chunks |
| audio | No | 0 | Audio packets |
| input | Yes | 3 | Mouse/keyboard events |

## Client Modules

| File | Purpose |
|------|---------|
| `state.js` | Shared state, constants, codec detection, metrics, clock sync |
| `network.js` | HTTP auth, WebRTC signaling, data channel handlers |
| `renderer.js` | WebGL2 rendering with aspect ratio letterboxing |
| `media.js` | VideoDecoder, AudioDecoder, AudioWorklet processor |
| `input.js` | Mouse/keyboard capture, RAF batching, pointer lock |
| `ui.js` | Settings panel, fullscreen, tabbed mode, stats overlay |

### Clock Synchronization

| Parameter | Value |
|-----------|-------|
| Ping interval | 200 ms |
| Sample count | 8 |
| Offset calculation | Median filter |
| RTT estimation | Median of samples |

### Frame Handling

| Parameter | Value |
|-----------|-------|
| Max buffered frames | 8 |
| Frame timeout | 200 ms |
| Max frame age (jitter) | 50 ms |
| Decode queue limit | 6 frames |

## UI Features

### Settings Panel

Access by clicking the right edge of the screen:

- **Logout** - Disconnect and clear session
- **Fullscreen** - Enter fullscreen with keyboard lock (Escape captured)
- **Audio** - Toggle system audio playback
- **Monitor** - Switch between displays
- **Frame Rate** - 15/30/60/120/144 or custom (1-240)
- **Codec** - AV1, H.265, or H.264 (shows HW/SW status)
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
| Throughput | FPS (actual/target/efficiency), bitrate, resolution, codec |
| Latency | RTT, frame age |
| Jitter | Frame interval mean and std dev |
| Decode | Average decode time, queue size, HW acceleration status |
| Render | Average render time, frame count |
| Network | Packet count (total/video/audio), average packet size |
| Drops | Dropped, timeout, late frames, decode errors |
| Audio | Packets received/decoded, buffer health, underruns, overflows |
| Input | Mouse moves, clicks, keystrokes |
| Session | Uptime, total frames, total data |

## Server Threads

| Thread | Priority | Purpose |
|--------|----------|---------|
| Main | Above Normal | HTTPS server, process priority |
| Encoder | Time Critical | Frame encoding and sending |
| Audio | Highest | Audio capture and encoding |
| Cursor | Below Normal | Cursor shape polling |

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
| picojson | Required by jwt-cpp |

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

## File Structure

```
SlipStream/
├── include/
│   ├── common.hpp      # Common includes, types, auth utilities
│   ├── capture.hpp     # Screen capture with WGC
│   ├── encoder.hpp     # NVENC video encoding
│   ├── webrtc.hpp      # WebRTC server implementation
│   ├── audio.hpp       # WASAPI audio capture + Opus
│   └── input.hpp       # Input handling + clipboard
├── src/
│   └── main.cpp        # Application entry point
├── client/
│   ├── index.html      # Web client HTML
│   ├── styles.css      # Stylesheet
│   └── js/
│       ├── state.js    # Shared state and metrics
│       ├── network.js  # WebRTC and HTTP
│       ├── renderer.js # WebGL2 rendering
│       ├── media.js    # Video/audio decoding
│       ├── input.js    # Input capture
│       └── ui.js       # UI controls
├── vcpkg.json          # Dependencies
├── CMakeLists.txt      # Build configuration
├── build.bat           # Build script
├── build_installer.bat # Installer build script
├── run.bat             # Run script
└── LICENSE.txt         # License
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| vcpkg not found | Set `VCPKG_ROOT` environment variable or install to `C:\vcpkg` |
| NVENC unavailable | Requires NVIDIA GPU with NVENC support (GTX 600+ series) |
| Connection refused | Check firewall for TCP 443 and UDP 50000-50020 |
| Black screen | Wait for keyframe or click canvas to focus |
| No audio | Click "Enable" in settings panel after first audio playback |
| Input not working | Click the video canvas to capture input focus |
| Certificate warning | Expected with self-signed certificate, click proceed |
| Kicked message | Another client connected (single client only) |

## Known Limitations

- Windows server only
- Single client connection (new client kicks existing)
- NVIDIA GPU required for encoding
- No file transfer support
- No custom cursor image sync (standard cursors only)

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial use requires separate license.

See LICENSE.txt for full terms.

## Copyright

(c) 2025-2026 Daniel Chrobak. All rights reserved.
