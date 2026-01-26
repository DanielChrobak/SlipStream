# SlipStream

Low-latency remote desktop streaming with AV1 hardware encoding and WebRTC.

## Overview

SlipStream captures the host display using the Windows Graphics Capture API and encodes it with NVIDIA NVENC AV1 (or libsvtav1 software fallback). Video streams to any modern browser via WebRTC data channels. System audio is captured and encoded with Opus. Full mouse and keyboard input is relayed back to the host.

## Features

- **AV1 Encoding** — NVIDIA NVENC hardware encoder with libsvtav1 software fallback
- **WebRTC Transport** — DTLS-encrypted peer-to-peer streaming via data channels
- **System Audio** — Opus-encoded audio capture at 48kHz stereo
- **Multi-Monitor** — Switch between displays during active sessions
- **Full Input** — Mouse movement, clicks, scroll wheel, and keyboard passthrough
- **Secure Auth** — SHA-256 hashed passwords with rate limiting and session tokens

## Requirements

### Server (Host)

- Windows 10/11 64-bit
- Visual Studio 2022 with C++ Desktop workload
- NVIDIA GPU with NVENC support (or CPU for software encoding)
- [vcpkg](https://github.com/microsoft/vcpkg)

### Client

- Modern browser with WebRTC and AV1 decoding (Chrome, Edge, Firefox)

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

First run prompts for:
- **Username** — 3-32 characters (letters, numbers, `_`, `-`)
- **Password** — 8+ characters with at least one letter and one number

Credentials are hashed and saved to `auth.json`.

### 4. Connect

1. Open browser to `http://<HOST_IP>:6060`
2. Enter server URL if prompted
3. Login with username and password

## Network

| Port | Protocol | Purpose |
|------|----------|---------|
| 6060 | TCP | HTTP server (web client, auth API, signaling) |
| 50000-50020 | UDP | WebRTC media (DTLS/SRTP) |

## Authentication

Passwords are hashed with SHA-256 using random salts. The plaintext password is never stored.

**auth.json format:**
```json
{
  "username": "admin",
  "passwordHash": "<64-char hex SHA-256>",
  "salt": "<64-char hex random salt>"
}
```

**Rate Limiting:**
- 5 failed attempts per IP within 15 minutes triggers a 30-minute lockout
- Successful login clears the attempt counter

**Sessions:**
- 64-character cryptographic tokens
- Expire after 24 hours or 4 hours of inactivity
- Stored in browser localStorage

## API Endpoints

| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/` | GET | No | Web client |
| `/api/auth` | POST | No | Login (returns session token) |
| `/api/session` | GET | Yes | Validate session |
| `/api/logout` | POST | Yes | Invalidate session |
| `/api/offer` | POST | Yes | WebRTC signaling |

## Encoder

The encoder tries NVENC first, then falls back to libsvtav1:

```cpp
const AVCodec* codec = avcodec_find_encoder_by_name("av1_nvenc");
if (!codec) codec = avcodec_find_encoder_by_name("libsvtav1");
if (!codec) throw std::runtime_error("No AV1 encoder available");
```

**NVENC settings:** Preset P1, ultra-low latency tune, CBR at 20 Mbps (max 40 Mbps), no B-frames, no lookahead, 2-second keyframe interval.

**libsvtav1 settings:** Preset 12, CRF 28, fast decode optimizations.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        SERVER (Windows)                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────┐  │
│  │   Screen     │───▶│     AV1      │───▶│     WebRTC       │  │
│  │   Capture    │    │   Encoder    │    │   Data Channel   │  │
│  │  (WGC API)   │    │ (NVENC/SVT)  │    │                  │  │
│  └──────────────┘    └──────────────┘    └────────┬─────────┘  │
│                                                   │             │
│  ┌──────────────┐    ┌──────────────┐             │             │
│  │    Audio     │───▶│    Opus      │─────────────┤             │
│  │   Capture    │    │   Encoder    │             │             │
│  │  (WASAPI)    │    │  (48kHz/2ch) │             │             │
│  └──────────────┘    └──────────────┘             │             │
│                                                   │             │
│  ┌──────────────┐    ┌──────────────┐             │             │
│  │    Input     │◀───│   HTTP API   │◀────────────┘             │
│  │   Inject     │    │   + Auth     │                           │
│  │  (SendInput) │    │              │                           │
│  └──────────────┘    └──────────────┘                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ DTLS + Unreliable Data Channel
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        CLIENT (Browser)                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────┐  │
│  │   WebRTC     │───▶│    AV1       │───▶│     WebGL2       │  │
│  │   Receive    │    │   Decoder    │    │    Renderer      │  │
│  └──────────────┘    └──────────────┘    └──────────────────┘  │
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐                          │
│  │    Opus      │───▶│    Audio     │                          │
│  │   Decoder    │    │   Playback   │                          │
│  └──────────────┘    └──────────────┘                          │
│                                                                 │
│  ┌──────────────┐                                              │
│  │   Mouse +    │───▶ (sent via data channel)                  │
│  │   Keyboard   │                                              │
│  └──────────────┘                                              │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Controls

**Mouse:** Move, left/right/middle click, scroll wheel (vertical and horizontal)

**Keyboard:** All keys passed through except OS shortcuts (Win key, etc.)

**Settings Panel:** Click the right edge of the screen to open. Options include:
- Disconnect
- Fullscreen (with keyboard lock for Escape)
- Audio enable/disable
- Monitor selection
- Frame rate (15/30/60/host native/client native)

## Client Files

| File | Description |
|------|-------------|
| `index.html` | Application shell with auth/connection modals |
| `styles.css` | UI styling |
| `js/network.js` | HTTP auth and WebRTC connection |
| `js/renderer.js` | WebGL2 video rendering |
| `js/media.js` | AV1 and Opus decoding |
| `js/input.js` | Mouse and keyboard handling |
| `js/state.js` | Shared application state |
| `js/ui.js` | Settings panel |

## Troubleshooting

**Encoder initialization failed**
- Update NVIDIA drivers for NVENC support
- Without NVIDIA GPU, libsvtav1 software encoding is used (higher CPU usage)

**Connection refused**
- Check firewall allows TCP 6060 and UDP 50000-50020
- Verify server is running (console shows `[WAIT]` or `[LIVE]`)

**Authentication failed**
- Check username and password
- After 5 failed attempts, IP is locked out for 30 minutes

**Video not displaying**
- Ensure browser supports AV1 decoding
- Check console for WebRTC connection errors
- Try refreshing and re-authenticating

**High latency**
- Use wired network
- Lower frame rate in settings
- Verify NVENC is being used (console shows `Encoder: av1_nvenc`)

## Building Installer

```batch
build_installer.bat
```

Creates NSIS installer (if installed) or ZIP package.

## Dependencies

Managed via vcpkg:
- libdatachannel (WebRTC)
- cpp-httplib with OpenSSL
- nlohmann-json
- opus
- openssl
- ffmpeg (nvcodec, aom, avcodec)

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial/company use requires separate license.

See [LICENSE.txt](LICENSE.txt) for full terms.

## Copyright

© 2025-2026 Daniel Chrobak
