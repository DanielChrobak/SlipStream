# SlipStream

Low-latency remote desktop streaming with hardware-accelerated encoding and WebRTC data channels.

## Overview

SlipStream captures the host display using Windows Graphics Capture API and encodes frames with NVIDIA NVENC (H.264 or AV1). Video streams to browsers via WebRTC unreliable data channels. System audio is captured via WASAPI loopback and encoded with Opus. Mouse and keyboard input is relayed back to the host.

## Features

- **Dual Codec** — H.264 and AV1 via NVIDIA NVENC
- **WebRTC Transport** — DTLS-encrypted unreliable data channels
- **System Audio** — Opus-encoded WASAPI loopback at 48kHz stereo
- **Multi-Monitor** — Live monitor switching with tabbed UI
- **Input Control** — Mouse, wheel, and keyboard with modifier support
- **Authentication** — PBKDF2-SHA256 (600k iterations) with rate limiting
- **GPU Sync** — D3D11 fence-based synchronization

## Requirements

### Server

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10/11 64-bit (build 1903+) |
| IDE | Visual Studio 2022 with C++20 Desktop workload |
| GPU | NVIDIA GPU with NVENC support |
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

First run prompts for credentials:
- **Username** — 3-32 alphanumeric characters (plus `_` and `-`)
- **Password** — 8+ characters with at least one letter and one digit

Credentials are hashed and saved to `auth.json`.

### 4. Connect

Open browser to `http://<HOST_IP>:6060` and enter credentials.

## Network Configuration

| Port | Protocol | Purpose |
|------|----------|---------|
| 6060 | TCP | HTTP server and WebRTC signaling |
| 50000-50020 | UDP | WebRTC media transport |

### Firewall

```powershell
netsh advfirewall firewall add rule name="SlipStream HTTP" dir=in action=allow protocol=tcp localport=6060
netsh advfirewall firewall add rule name="SlipStream WebRTC" dir=in action=allow protocol=udp localport=50000-50020
```

## Authentication

### Password Security

- **600,000 PBKDF2 iterations** with SHA-256
- **16-byte random salt**
- **32-byte derived key** stored as hex

### Rate Limiting

| Threshold | Action |
|-----------|--------|
| 5 failed attempts in 15 minutes | 30-minute IP lockout |
| Successful login | Clears attempt counter |

### Session

- JWT tokens with HS256 signing
- 24-hour expiry
- Stored in browser localStorage

## API Endpoints

| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/` | GET | No | Web client |
| `/styles.css` | GET | No | Stylesheet |
| `/js/*.js` | GET | No | JavaScript modules |
| `/api/auth` | POST | No | Login with `{username, password}` |
| `/api/session` | GET | Bearer | Validate session |
| `/api/logout` | POST | No | Logout |
| `/api/offer` | POST | Bearer | WebRTC offer exchange |

## Video Pipeline

### Capture

- Windows Graphics Capture API
- 6-texture pool with fence sync
- 4-frame ring buffer for encoder handoff

### Encoding

| Setting | H.264 | AV1 |
|---------|-------|-----|
| Preset | P1 | P1 |
| Tune | Ultra-low latency | Ultra-low latency |
| Rate Control | VBR | VBR |
| CQ Level | 23 | 28 |
| Keyframe Interval | 2 seconds | 2 seconds |
| B-Frames | Disabled | Disabled |

Bitrate formula: `0.18085 × width × height × fps`

### Transport

- 1400-byte chunks with 21-byte header
- 256KB buffer threshold
- Max 3 frames queued
- Unreliable, unordered delivery

### Packet Header (21 bytes)

| Field | Size | Description |
|-------|------|-------------|
| timestamp | 8 | Capture timestamp (µs) |
| encodeTimeUs | 4 | Encode duration |
| frameId | 4 | Frame sequence number |
| chunkIndex | 2 | Chunk index |
| totalChunks | 2 | Total chunks in frame |
| frameType | 1 | 1=key, 0=delta |

## Audio Pipeline

### Capture

| Property | Value |
|----------|-------|
| API | WASAPI Loopback |
| Sample Rate | 48,000 Hz |
| Channels | Stereo |
| Frame Duration | 10 ms (480 samples) |

### Encoding

| Property | Value |
|----------|-------|
| Codec | Opus |
| Application | Restricted Low Delay |
| Bitrate | 96 kbps |
| Complexity | 3 |

### Transport

- 16-byte header + Opus payload
- Max 4 packets queued
- 128KB buffer threshold

### Audio Header (16 bytes)

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x41554449` |
| timestamp | 8 | Capture timestamp |
| samples | 2 | Sample count |
| dataLength | 2 | Payload size |

## Input Protocol

### Mouse Move (12 bytes)

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x4D4F5645` |
| x | 4 | Normalized X (float 0-1) |
| y | 4 | Normalized Y (float 0-1) |

### Relative Mouse Move (8 bytes)

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x4D4F5652` |
| dx | 2 | Delta X |
| dy | 2 | Delta Y |

### Mouse Button (6 bytes)

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x4D42544E` |
| button | 1 | 0=left, 1=right, 2=middle, 3-4=x |
| action | 1 | 1=down, 0=up |

### Mouse Wheel (8 bytes)

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x4D57484C` |
| deltaX | 2 | Horizontal scroll |
| deltaY | 2 | Vertical scroll |

### Keyboard (10 bytes)

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x4B455920` |
| keyCode | 2 | JavaScript keyCode |
| scanCode | 2 | Hardware scan code |
| action | 1 | 1=down, 0=up |
| modifiers | 1 | Ctrl(1) Alt(2) Shift(4) Meta(8) |

### Rate Limits

| Type | Max per Second |
|------|----------------|
| Mouse moves | 500 |
| Clicks | 50 |
| Keystrokes | 100 |

### Blocked Keys

- Windows key (left/right)
- Ctrl+Alt+Delete

## Client Architecture

| File | Purpose |
|------|---------|
| `state.js` | Shared state, constants, metrics, clock sync |
| `network.js` | HTTP auth, WebRTC signaling, message routing |
| `renderer.js` | WebGL2 rendering with letterboxing |
| `media.js` | VideoDecoder/AudioDecoder, keyframe retry |
| `input.js` | Mouse/keyboard capture and transmission |
| `ui.js` | Settings panel, tabbed mode, fullscreen |

### Clock Sync

- Ping messages every 200ms
- 8-sample median filter for offset calculation
- RTT-based latency estimation

### Jitter Buffer

| Setting | Value |
|---------|-------|
| Max frames | 2 |
| Max age | 50 ms |
| Frame timeout | 200 ms |

## UI Features

### Settings Panel

Click right edge to open:

- **Logout** — Disconnect and clear session
- **Fullscreen** — Enter fullscreen with keyboard lock
- **Audio** — Toggle system audio
- **Monitor** — Switch displays
- **Frame Rate** — 15/30/60/120/144/custom FPS
- **Codec** — H.264 or AV1
- **Tabbed Mode** — Monitor tabs at top
- **Debug Stats** — Performance overlay
- **Relative Mouse** — Pointer lock mode

### Debug Stats Overlay

Shows real-time metrics:
- FPS, bitrate, resolution, codec
- RTT, frame age, jitter
- Decode/render times
- Network packets
- Audio buffer health
- Drop counts

## Server Threads

| Thread | Priority | Purpose |
|--------|----------|---------|
| Main | Above Normal | HTTP server |
| Encoder | Time Critical | Frame encoding |
| Audio | Highest | Audio capture/encode |

## Data Channel Configuration

| Channel | Ordered | MaxRetransmits |
|---------|---------|----------------|
| control | Yes | 3 |
| video | No | 0 |
| audio | No | 0 |
| input | Yes | 3 |

## Dependencies

Managed via vcpkg:

| Package | Purpose |
|---------|---------|
| libdatachannel | WebRTC |
| cpp-httplib[openssl] | HTTP server |
| nlohmann-json | JSON |
| opus | Audio encoding |
| openssl | Cryptography |
| ffmpeg[nvcodec,avcodec] | Video encoding |
| jwt-cpp | JWT tokens |

## Building Installer

```batch
build_installer.bat
```

Creates `SlipStream-1.0.0-win64.exe` (NSIS) or `.zip` (fallback).

## Troubleshooting

| Problem | Solution |
|---------|----------|
| vcpkg not found | Set `VCPKG_ROOT` or install to `C:\vcpkg` |
| NVENC unavailable | Requires NVIDIA GPU with NVENC |
| Connection refused | Check firewall for TCP 6060, UDP 50000-50020 |
| Black screen | Wait for keyframe or refresh |
| No audio | Click Enable in settings |
| Input not working | Click canvas to capture |

## Limitations

- Windows server only
- Single client
- NVIDIA GPU required
- No clipboard/file transfer

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial use requires separate license.

## Copyright

(c) 2025-2026 Daniel Chrobak. All rights reserved.
