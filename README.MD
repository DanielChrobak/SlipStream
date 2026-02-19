# SlipStream

Low-latency remote desktop streaming over WebRTC with hardware-accelerated encoding.

## Overview

SlipStream captures the Windows desktop using the Windows Graphics Capture API and encodes frames in real-time using hardware encoders (NVIDIA NVENC, Intel QSV, or AMD AMF). Video and audio are streamed to web browsers via WebRTC data channels with DTLS encryption. The browser client renders frames using WebGL2 and decodes video/audio using the WebCodecs API. Bidirectional microphone support enables voice communication from the client to the server via VB-Cable.

## Architecture

```
+------------------------------------------------------------------------------+
|                              SERVER (Windows)                                |
+------------------------------------------------------------------------------+
|  Screen Capture         Encoder             WebRTC              Audio        |
|  +--------------+    +--------------+    +--------------+    +----------+    |
|  | WGC API      |--->| NVENC/QSV/   |--->| Data Channel |<-->| WASAPI   |    |
|  | D3D11 Fence  |    | AMF          |    | libdatachan  |    | Opus     |    |
|  | 6-tex pool   |    | AV1/H265/264 |    | 5 channels   |    | 96 kbps  |    |
|  +--------------+    +--------------+    +--------------+    +----------+    |
|                                                 |                  |         |
|                                                 |            +----------+    |
|                                                 |            | VB-Cable |    |
|                                                 |            | Playback |    |
|                                                 |            +----------+    |
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
|                                                                    |         |
|  +--------------+                                            +----------+    |
|  | AudioEncoder |<-------------------------------------------|  Mic     |    |
|  | Opus 32kbps  |                                            | Capture  |    |
|  +--------------+                                            +----------+    |
|                                                                              |
|  Input Handler -----> Mouse/Keyboard -----> Data Channel -----> Server       |
+------------------------------------------------------------------------------+
```

## Features

- **Multi-Vendor GPU Support** - NVIDIA NVENC, Intel QSV, AMD AMF with automatic detection and fallback
- **Triple Codec Support** - AV1, H.265, and H.264 with automatic capability detection
- **Hardware Decoding** - WebCodecs API with automatic HW/SW fallback
- **System Audio** - WASAPI loopback capture with Opus encoding at 96 kbps
- **Microphone Support** - Client-to-server voice via WebCodecs AudioEncoder and VB-Cable
- **Multi-Monitor** - Live monitor switching with tabbed UI
- **Clipboard Sync** - Bidirectional text clipboard (Ctrl+C/V)
- **Relative Mouse** - Pointer lock mode for games and full-screen apps
- **Cursor Sync** - Remote cursor shape updates (14 standard cursor types)
- **Session Auth** - PBKDF2-SHA256 password hashing with JWT tokens
- **Rate Limiting** - Brute-force protection with IP lockout
- **Auto SSL** - Self-signed certificate generation on first run
- **Debug Stats** - Real-time performance overlay with comprehensive metrics

## Requirements

### Server

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10/11 64-bit (build 1903+) |
| GPU | NVIDIA (NVENC), Intel (QSV), or AMD (AMF) GPU |
| IDE | Visual Studio 2022 with C++20 Desktop workload |
| Package Manager | [vcpkg](https://github.com/microsoft/vcpkg) |
| Microphone Output | VB-Cable (optional, for client mic playback) |

### Client

| Component | Requirement |
|-----------|-------------|
| Browser | Chrome 94+, Edge 94+, Firefox 98+ |
| APIs | WebRTC, WebCodecs (VideoDecoder, AudioDecoder, AudioEncoder), WebGL2, AudioWorklet |

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
- Generates self-signed SSL certificate (`server.crt`, `server.key`) in `%APPDATA%\\SlipStream`
- Saves hashed credentials to `%APPDATA%\\SlipStream\\auth.json`

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

Passwords are never stored in plain text. Only the salt and derived hash are saved in `%APPDATA%\\SlipStream\\auth.json`.

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

To use custom certificates, replace `%APPDATA%\\SlipStream\\server.crt` and `%APPDATA%\\SlipStream\\server.key` before starting.

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
| Frame Buffer | 4-frame ring buffer with generation tracking |
| Cursor | Optional capture in stream |
| Border | Disabled (if OS supports) |

### GPU Vendor Detection

The encoder automatically detects the GPU vendor and selects the appropriate encoder:

| Vendor ID | Vendor | Encoder |
|-----------|--------|---------|
| 0x10DE | NVIDIA | NVENC |
| 0x8086 | Intel | QSV |
| 0x1002 | AMD | AMF |

If the primary GPU's encoder fails, the system falls back to other available encoders.

### Encoding

| Setting | H.264 | H.265 | AV1 |
|---------|-------|-------|-----|
| NVIDIA | h264_nvenc | hevc_nvenc | av1_nvenc |
| Intel | h264_qsv | hevc_qsv | av1_qsv |
| AMD | h264_amf | hevc_amf | av1_amf |

#### NVIDIA NVENC Configuration

| Setting | Value |
|---------|-------|
| Preset | P1 (fastest) |
| Tune | Ultra-low latency |
| Rate Control | VBR |
| CQ Level | 23 (H.264), 25 (H.265), 28 (AV1) |
| Zero Latency | Enabled |
| B-Frames | 0 |

#### Intel QSV Configuration

| Setting | Value |
|---------|-------|
| Preset | veryfast |
| Look Ahead | Disabled |
| Async Depth | 1 |
| Low Power | Enabled |
| Global Quality | 23 (H.264), 25 (H.265), 28 (AV1) |

#### AMD AMF Configuration

| Setting | Value |
|---------|-------|
| Usage | Ultra-low latency |
| Quality | Speed |
| Rate Control | VBR latency |
| Header Insertion | GOP |
| QP | 23 (H.264), 25 (H.265), 28 (AV1) |

**Bitrate Formula:** `0.18085 × width × height × fps` bps

Example: 1920×1080 @ 60fps ≈ 22.5 Mbps

### Transport

| Parameter | Value |
|-----------|-------|
| Chunk Size | 1400 bytes max (1379 payload + 21 header) |
| Header Size | 21 bytes |
| Video Buffer | 256 KB threshold |
| Max Queue | 3 frames worth of chunks |
| Delivery | Ordered, limited retransmits |

### Video Packet Header (21 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | timestamp | Capture timestamp (microseconds) |
| 8 | 4 | encodeTimeUs | Encode duration (microseconds) |
| 12 | 4 | frameId | Frame sequence number |
| 16 | 2 | chunkIndex | Current chunk index |
| 18 | 2 | totalChunks | Total chunks in frame |
| 20 | 1 | frameType | 1=keyframe, 0=delta |

## Audio Pipeline (Server to Client)

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
| Delivery | Ordered, 1 retransmit |

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
| Prebuffer Threshold | 1440 samples (30ms) |
| Target Buffer | 2400 samples (50ms) |
| Max Buffer | 4800 samples (100ms) |

## Microphone Pipeline (Client to Server)

### Client Capture

| Parameter | Value |
|-----------|-------|
| API | MediaDevices.getUserMedia |
| Sample Rate | 48,000 Hz |
| Channels | Mono |
| Frame Duration | 10 ms (480 samples) |
| Processing | Echo cancellation, noise suppression, auto gain |

### Client Encoding

| Parameter | Value |
|-----------|-------|
| Codec | Opus (via WebCodecs AudioEncoder) |
| Application | VoIP |
| Bitrate | 32 kbps |
| Complexity | 5 |
| Signal Type | Voice |
| Frame Duration | 10 ms |

### Transport

| Parameter | Value |
|-----------|-------|
| Delivery | Ordered, 1 retransmit |
| Max Queue | 20 packets |

### Microphone Packet Header (16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | 0x4D494344 ("MICD") |
| 4 | 8 | timestamp | Capture timestamp |
| 12 | 2 | samples | Sample count (480) |
| 14 | 2 | dataLength | Opus payload size |

### Server Playback

| Component | Detail |
|-----------|--------|
| Output Device | VB-Cable Input (auto-detected) |
| Decoder | Opus (libopus) |
| Resampling | Linear interpolation if device rate differs |
| Fallback | Default audio endpoint if VB-Cable unavailable |

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

## Control Messages

| Message | Magic | Size | Description |
|---------|-------|------|-------------|
| PING | 0x504E4750 | 16/24 | Clock synchronization |
| HOST_INFO | 0x484F5354 | 6 | Host display refresh rate |
| FPS_SET | 0x46505343 | 7 | Set target frame rate |
| FPS_ACK | 0x46505341 | 7 | Frame rate acknowledgment |
| CODEC_SET | 0x434F4443 | 5 | Set video codec |
| CODEC_ACK | 0x434F4441 | 5 | Codec acknowledgment |
| CODEC_CAPS | 0x434F4350 | 5 | Server codec capabilities bitmask |
| REQUEST_KEY | 0x4B455952 | 4 | Request keyframe |
| MONITOR_LIST | 0x4D4F4E4C | Variable | Monitor enumeration |
| MONITOR_SET | 0x4D4F4E53 | 5 | Switch monitor |
| AUDIO_ENABLE | 0x41554445 | 5 | Enable/disable audio streaming |
| MIC_ENABLE | 0x4D494345 | 5 | Enable/disable mic streaming |
| CURSOR_CAPTURE | 0x43555243 | 5 | Toggle cursor capture in video |
| CLIPBOARD_GET | 0x434C4754 | 4 | Request clipboard contents |
| CLIPBOARD_DATA | 0x434C4950 | 8+N | Clipboard text transfer |
| KICKED | 0x4B49434B | 4 | Client disconnected by server |

### Codec Capabilities (CODEC_CAPS)

The server sends a bitmask indicating supported codecs:
- Bit 0 (0x01): AV1 supported
- Bit 1 (0x02): H.265 supported
- Bit 2 (0x04): H.264 supported

The client uses this to enable/disable codec options in the UI.

## WebRTC Data Channels

| Channel | Ordered | MaxRetransmits | Purpose |
|---------|---------|----------------|---------|
| control | Yes | 3 | Commands, ping/pong, monitor list |
| video | Yes | 1 | Video frame chunks |
| audio | Yes | 1 | Audio packets |
| input | Yes | 3 | Mouse/keyboard events |
| mic | Yes | 1 | Microphone audio packets |

## Client Modules

| File | Purpose |
|------|---------|
| `state.js` | Shared state, constants, codec detection, metrics, clock sync |
| `network.js` | HTTP auth, WebRTC signaling, data channel handlers |
| `renderer.js` | WebGL2 rendering with aspect ratio letterboxing |
| `media.js` | VideoDecoder, AudioDecoder, AudioWorklet processor |
| `input.js` | Mouse/keyboard capture, RAF batching, pointer lock |
| `ui.js` | Settings panel, fullscreen, tabbed mode, stats overlay |
| `mic.js` | Microphone capture, AudioEncoder, packet transmission |

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
- **Mic** - Toggle microphone transmission to server
- **Monitor** - Switch between displays
- **Frame Rate** - 15/30/60/120/144 or custom (1-240)
- **Codec** - AV1, H.265, or H.264 (shows HW/SW status and host availability)
- **Tabbed Mode** - Monitor tabs at top of screen
- **Debug Stats** - Real-time performance overlay
- **Relative Mouse** - Pointer lock mode for gaming
- **Clipboard Sync** - Enable Ctrl+C/V synchronization

### Tabbed Mode

When enabled, displays a tab strip showing all monitors with:
- Monitor name (EDID friendly name)
- Resolution and refresh rate
- Primary indicator (star icon)
- Click to switch monitors

### Debug Stats Overlay

Real-time metrics display organized into sections:

| Section | Metrics |
|---------|---------|
| Throughput | FPS (actual/target with efficiency %), bitrate, resolution, codec (with HW/SW indicator) |
| Latency | RTT, frame age |
| Jitter | Frame interval mean and standard deviation |
| Decode | Average decode time, queue size, HW acceleration status |
| Render | Average render time, frame count |
| Network | Packet count (total, video, audio), average packet size |
| Drops | Dropped frames, timeouts, late frames, decode errors |
| Audio | Packets received/decoded, buffer level (ms) |
| Audio Drops | Dropped, underruns, overflows |
| Input | Mouse moves/clicks, keystrokes |
| Session | Uptime, total frames, total data transferred |

## Server Components

### Thread Architecture

| Thread | Priority | Purpose |
|--------|----------|---------|
| Main | Above Normal | HTTPS server, process priority |
| Encoder | Time Critical | Frame encoding and sending |
| Audio | Highest | Audio capture and encoding |
| Cursor | Below Normal | Cursor shape polling |
| Wiggle | Normal | Mouse cursor initialization after monitor switch |

### Server Header Files

| File | Purpose |
|------|---------|
| `common.hpp` | Types, auth utilities, monitor enumeration, SSL generation |
| `capture.hpp` | Screen capture with WGC, texture pool, frame slot |
| `encoder.hpp` | Hardware video encoding via FFmpeg (NVENC/QSV/AMF) |
| `webrtc.hpp` | WebRTC server, data channels |
| `audio.hpp` | WASAPI audio capture + Opus encoding |
| `input.hpp` | Input handling, keyboard/mouse injection, clipboard |
| `mic.hpp` | VB-Cable microphone playback |

## Dependencies

Managed via vcpkg (`vcpkg.json`):

| Package | Purpose |
|---------|---------|
| libdatachannel | WebRTC implementation |
| cpp-httplib[openssl] | HTTPS server |
| nlohmann-json | JSON parsing |
| opus | Audio encoding/decoding |
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
│   ├── encoder.hpp     # Hardware video encoding (NVENC/QSV/AMF)
│   ├── webrtc.hpp      # WebRTC server implementation
│   ├── audio.hpp       # WASAPI audio capture + Opus encoding
│   ├── input.hpp       # Input handling + clipboard
│   └── mic.hpp         # VB-Cable microphone playback
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
│       ├── ui.js       # UI controls
│       └── mic.js      # Microphone capture and encoding
├── vcpkg.json          # Dependencies
├── CMakeLists.txt      # Build configuration
├── build.bat           # Build script
├── build_installer.bat # Installer build script
├── run.bat             # Run script
├── LICENSE.md          # Styled license text
└── LICENSE-installer.txt # Installer license text
```

## Microphone Setup (VB-Cable)

To enable client microphone audio on the server:

1. Install [VB-Cable Virtual Audio Device](https://vb-audio.com/Cable/)
2. The server automatically detects "CABLE Input" as the output device
3. Configure your application to use "CABLE Output" as its microphone input

If VB-Cable is not installed, the server falls back to the default audio endpoint.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| vcpkg not found | Set `VCPKG_ROOT` environment variable or install to `C:\vcpkg` |
| No hardware encoder | Requires NVIDIA (GTX 600+), Intel (6th gen+), or AMD (RX 400+) GPU |
| Connection refused | Check firewall for TCP 443 and UDP 50000-50020 |
| Black screen | Wait for keyframe or click canvas to focus |
| No audio | Click "Enable" in settings panel after first audio playback |
| Mic not working | Ensure browser has microphone permission, check VB-Cable installation |
| Input not working | Click the video canvas to capture input focus |
| Certificate warning | Expected with self-signed certificate, click proceed |
| Kicked message | Another client connected (single client only) |
| Monitor switch issues | Frames from previous monitor are automatically discarded |
| Codec grayed out | Either client browser or server GPU doesn't support that codec |

## Known Limitations

- Windows server only
- Single client connection (new client kicks existing)
- Hardware GPU required for encoding (NVIDIA, Intel, or AMD)
- No custom cursor image sync (standard cursors only)
- Microphone requires VB-Cable for full functionality
- WebCodecs AudioEncoder required for client microphone

## Codec Support Detection

### Server-Side

The server probes for encoder support on startup:
1. Detects primary GPU vendor (NVIDIA/Intel/AMD)
2. Tests each codec (AV1, H.265, H.264) with the vendor's encoder
3. Falls back to other vendors if primary fails
4. Sends codec capabilities bitmask to client

### Client-Side

The client automatically detects codec support on connection:
1. Tests hardware acceleration for each codec (AV1, H.265, H.264)
2. Falls back to software decoding if hardware unavailable
3. Intersects with server capabilities to determine available options
4. Displays HW/SW status and availability in codec dropdown

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial use requires separate license.

- Styled/readable version: [LICENSE.md](LICENSE.md)
- Installer-rendered copy: [LICENSE-installer.txt](LICENSE-installer.txt)

## Copyright

(c) 2025-2026 Daniel Chrobak. All rights reserved.

