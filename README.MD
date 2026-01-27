# SlipStream

Low-latency remote desktop streaming with hardware-accelerated video encoding and WebRTC data channels.

## Overview

SlipStream captures the host display using the Windows Graphics Capture API and encodes frames with NVIDIA NVENC (H.264 or AV1). Video streams to modern browsers via WebRTC unreliable data channels for minimal latency. System audio is captured via WASAPI loopback and encoded with Opus. Full mouse and keyboard input is relayed back to the host with rate limiting and security filtering.

## Features

- **Dual Codec Support** — H.264 and AV1 via NVIDIA NVENC hardware encoding
- **WebRTC Data Channels** — DTLS-encrypted unreliable transport for lowest latency
- **System Audio** — Opus-encoded WASAPI loopback capture at 48kHz stereo
- **Multi-Monitor** — Live monitor switching with tabbed browser UI
- **Full Input Control** — Mouse, scroll wheel, and keyboard with modifier support
- **Secure Authentication** — PBKDF2-SHA256 (600k iterations) with rate limiting
- **GPU Synchronization** — D3D11 fence-based sync with flush fallback
- **Clock Synchronization** — Client-server time sync for accurate latency measurement
- **Adaptive Frame Pacing** — Queue-based sending with overflow protection

## Requirements

### Server (Host Machine)

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10/11 64-bit (build 1903+) |
| IDE | Visual Studio 2022 with C++20 Desktop workload |
| GPU | NVIDIA GPU with NVENC support (required) |
| Package Manager | [vcpkg](https://github.com/microsoft/vcpkg) |

### Client (Browser)

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

Credentials are hashed with PBKDF2-SHA256 and saved to `auth.json`.

### 4. Connect

1. Open browser to `http://<HOST_IP>:6060`
2. Enter username and password
3. Stream begins automatically after authentication

## Network Configuration

| Port | Protocol | Purpose |
|------|----------|---------|
| 6060 | TCP | HTTP server (web client, REST API, WebRTC signaling) |
| 50000-50020 | UDP | WebRTC media transport (DTLS/SCTP data channels) |

### Firewall Rules

```powershell
# Allow HTTP server
netsh advfirewall firewall add rule name="SlipStream HTTP" dir=in action=allow protocol=tcp localport=6060

# Allow WebRTC UDP
netsh advfirewall firewall add rule name="SlipStream WebRTC" dir=in action=allow protocol=udp localport=50000-50020
```

## Authentication

### Password Security

Passwords are hashed using PBKDF2-SHA256 with:
- **600,000 iterations** — Resistant to brute-force attacks
- **16-byte random salt** — Prevents rainbow table attacks
- **32-byte derived key** — Stored as 64-character hex string

The plaintext password is never stored or logged.

### Rate Limiting

| Threshold | Action |
|-----------|--------|
| 5 failed attempts in 15 minutes | 30-minute IP lockout |
| Successful login | Clears attempt counter |
| Lockout expiry | Automatic after 30 minutes |

### Session Management

| Property | Value |
|----------|-------|
| Token length | 64 characters (cryptographic random) |
| Absolute timeout | 24 hours |
| Inactivity timeout | 4 hours |
| Storage | Browser localStorage |

### auth.json Format

```json
{
  "username": "admin",
  "passwordHash": "<64-char hex SHA-256 derived key>",
  "salt": "<32-char hex random salt>"
}
```

## API Reference

| Endpoint | Method | Auth | Request | Response |
|----------|--------|------|---------|----------|
| `/` | GET | No | — | Web client HTML |
| `/styles.css` | GET | No | — | Stylesheet |
| `/js/*.js` | GET | No | — | JavaScript modules |
| `/api/auth` | POST | No | `{username, password}` | `{token, expiresIn}` |
| `/api/session` | GET | Bearer | — | `{valid, username}` |
| `/api/logout` | POST | Bearer | — | `{success}` |
| `/api/offer` | POST | Bearer | `{sdp, type}` | `{sdp, type}` |

### Error Responses

```json
{"error": "Invalid credentials", "remainingAttempts": 3}
{"error": "Too many attempts", "lockoutSeconds": 1800}
{"error": "Session expired"}
{"error": "Authentication required"}
```

## Video Pipeline

### Capture Stage

```
Windows Graphics Capture API
         │
         ▼
┌─────────────────────┐
│ WGC Frame Pool (4)  │  WinRT Direct3D11CaptureFramePool
└─────────────────────┘
         │
         ▼
┌─────────────────────┐
│  Texture Pool (6)   │  D3D11 staging textures with fence sync
└─────────────────────┘
         │
         ▼
┌─────────────────────┐
│   Frame Slot (4)    │  Lock-free ring buffer for encoder handoff
└─────────────────────┘
```

### Encoding Stage

| Setting | H.264 (NVENC) | AV1 (NVENC) |
|---------|---------------|-------------|
| Preset | P1 (fastest) | P1 (fastest) |
| Tune | Ultra-low latency | Ultra-low latency |
| Rate Control | VBR | VBR |
| CQ Level | 23 | 28 |
| Target Bitrate | 20 Mbps | 20 Mbps |
| Max Bitrate | 40 Mbps | 40 Mbps |
| B-Frames | Disabled | Disabled |
| Lookahead | Disabled | Disabled |
| Keyframe Interval | 2 seconds | 2 seconds |

### Transport Stage

```
┌─────────────────────┐
│  Encoded Frame      │
└─────────────────────┘
         │
         ▼  Split into 1400-byte chunks (21-byte header)
┌─────────────────────┐
│   Packet Header     │  timestamp (8) + encodeTime (4) + frameId (4) +
│   + Chunk Data      │  chunkIndex (2) + totalChunks (2) + frameType (1)
└─────────────────────┘
         │
         ▼  Queue with overflow protection (max 3 frames)
┌─────────────────────┐
│   Send Queue        │  256KB buffer threshold, paced drain
└─────────────────────┘
         │
         ▼
┌─────────────────────┐
│  WebRTC DataChannel │  Unreliable, unordered (maxRetransmits: 0)
└─────────────────────┘
```

## Audio Pipeline

### Capture

| Property | Value |
|----------|-------|
| API | WASAPI Loopback |
| Sample Rate | 48,000 Hz |
| Channels | Stereo (max 2) |
| Frame Duration | 20 ms (960 samples) |

### Encoding

| Property | Value |
|----------|-------|
| Codec | Opus |
| Application | Restricted Low Delay |
| Bitrate | 128 kbps |
| Complexity | 5 |

### Transport

```
┌─────────────────────┐
│  Audio Packet       │  16-byte header + Opus payload
│  Header:            │  magic (4) + timestamp (8) +
│                     │  samples (2) + dataLength (2)
└─────────────────────┘
```

## Input Handling

### Mouse Events

| Event | Protocol Bytes | Description |
|-------|----------------|-------------|
| Move | 12 | magic (4) + x (4, float) + y (4, float) |
| Button | 6 | magic (4) + button (1) + action (1) |
| Wheel | 8 | magic (4) + deltaX (2) + deltaY (2) |

### Keyboard Events

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | `0x4B455920` ("KEY ") |
| keyCode | 2 | JavaScript keyCode |
| scanCode | 2 | Hardware scan code (reserved) |
| action | 1 | 1 = down, 0 = up |
| modifiers | 1 | Ctrl (1) + Alt (2) + Shift (4) + Meta (8) |

### Rate Limiting

| Input Type | Max per Second |
|------------|----------------|
| Mouse moves | 500 |
| Mouse clicks | 50 |
| Keystrokes | 100 |

### Blocked Combinations

- Windows key (left/right)
- Ctrl+Alt+Delete

## Client Architecture

### Module Structure

| File | Purpose |
|------|---------|
| `state.js` | Shared state, constants, clock sync, metrics |
| `network.js` | HTTP auth, WebRTC signaling, message routing |
| `renderer.js` | WebGL2 video rendering with letterboxing |
| `media.js` | VideoDecoder/AudioDecoder with keyframe retry |
| `input.js` | Mouse/keyboard capture and transmission |
| `ui.js` | Settings panel, tabbed mode, fullscreen |

### Clock Synchronization

The client synchronizes time with the server using ping messages:

```
Client Send Time ─────────────────────▶ Server Time
       │                                    │
       │◀────────── RTT / 2 ───────────────│
       │                                    │
       ▼                                    ▼
    Offset = ServerTime - (ClientSend + RTT/2)
```

- 8 samples maintained with median filtering
- Drift tracking for long sessions
- Used for end-to-end latency calculation
- Ping interval: 200ms

### Jitter Buffer

| Setting | Value |
|---------|-------|
| Max frames | 2 |
| Max age | 50 ms |
| Frame timeout | 100 ms |

## Server Metrics

The server logs detailed metrics every second when streaming:

```
━━━ [42s] [LIVE] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[THROUGHPUT] FPS: 60/60 (100.0%) | Bitrate: 18.45 Mbps | V:60 A:50 | Res: 2560x1440 | Codec: H.264
[PIPELINE] Total: 8.23ms | Cap:1.12 + Hand:0.45 + Enc:4.56 + GPU:1.23 + Net:0.87ms
[CAPTURE] Captured: 60/60 arrived | Interval: 16.45/16.67/16.89ms | Miss:0 Skip:0
[ENCODER] Handoff: 0.32/0.45/0.67ms | Encode: 3.45/4.56/5.67ms
[NETWORK] Queue: avg:2 max:4 depth:1 | Burst: 12/18 avg/max | Drains:60
[INPUT] Mouse: 245 moves, 3 clicks | Keys: 12
[SESSION] Uptime: 42s | Frames: 2520 (60.0 avg) | Data: 95.23MB (18.12 Mbps avg)
```

## Client Metrics

The browser console displays detailed metrics every second:

```
━━━ [15s] [LIVE] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[THROUGHPUT] FPS: 60/60 (100.0%) | Bitrate: 18.45 Mbps | Frames: 60 recv, 60 dec, 60 rend
[LATENCY] E2E: 12.5ms | RTT: 8.2ms (6.1/10.3) | FrameAge: 8.3ms (5.2/12.1)
[JITTER] Queue: 0.85 (max:2) | Interval: 16.7±0.8ms | LocalAge: 2.1ms
[DECODE] Count: 60 | Time: 1.23ms (0.95/2.10) | Queue: 1 | HW: HW
[RENDER] Count: 60 | Time: 0.45ms (0.32/0.78) | Resolution: 2560x1440
[NETWORK] Packets: 1250 (V:1200 A:48 C:2) | Size: 1180B avg (24-1421)
```

## UI Features

### Settings Panel

Access by clicking the right edge of the screen:

- **Logout** — Disconnect and clear session
- **Fullscreen** — Enter fullscreen with keyboard lock (Escape captured)
- **Audio** — Toggle system audio streaming
- **Monitor** — Switch between host displays
- **Frame Rate** — Select 15/30/60/host native/client native FPS
- **Codec** — Switch between H.264 and AV1 (if supported)
- **Tabbed Mode** — Show monitor tabs at top of screen

### Tabbed Mode

When enabled, displays a Chrome-style tab bar showing all available monitors:
- Click tabs to switch monitors
- Shows resolution and primary indicator
- Preference saved to localStorage

### Codec Selection

The client automatically detects codec support and prefers:
1. AV1 with hardware acceleration
2. AV1 with software decoding
3. H.264 with hardware acceleration
4. H.264 with software decoding

User preference is saved to localStorage.

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         SERVER (Windows)                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌────────────────┐    ┌────────────────┐    ┌──────────────────┐  │
│  │  Screen        │───▶│  H.264/AV1     │───▶│    WebRTC        │  │
│  │  Capture       │    │  Encoder       │    │    Server        │  │
│  │  (WGC API)     │    │  (NVENC)       │    │  (DataChannel)   │  │
│  └────────────────┘    └────────────────┘    └────────┬─────────┘  │
│         │                      │                      │            │
│  ┌──────┴──────┐        ┌──────┴──────┐               │            │
│  │  Texture    │        │  D3D11      │               │            │
│  │  Pool (6)   │        │  Fence Sync │               │            │
│  └─────────────┘        └─────────────┘               │            │
│                                                       │            │
│  ┌────────────────┐    ┌────────────────┐             │            │
│  │  Audio         │───▶│    Opus        │─────────────┤            │
│  │  Capture       │    │    Encoder     │             │            │
│  │  (WASAPI)      │    │  (128kbps)     │             │            │
│  └────────────────┘    └────────────────┘             │            │
│                                                       │            │
│  ┌────────────────┐    ┌────────────────┐             │            │
│  │  Input         │◀───│  HTTP Server   │◀────────────┘            │
│  │  Handler       │    │  + Auth API    │                          │
│  │  (SendInput)   │    │  (Port 6060)   │                          │
│  └────────────────┘    └────────────────┘                          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                               │
                               │ DTLS + Unreliable DataChannel
                               │ (UDP 50000-50020)
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         CLIENT (Browser)                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌────────────────┐    ┌────────────────┐    ┌──────────────────┐  │
│  │  WebRTC        │───▶│  H.264/AV1     │───▶│    WebGL2        │  │
│  │  Receiver      │    │  Decoder       │    │    Renderer      │  │
│  │  (DataChannel) │    │ (VideoDecoder) │    │                  │  │
│  └────────────────┘    └────────────────┘    └──────────────────┘  │
│         │                                                          │
│         │              ┌────────────────┐    ┌──────────────────┐  │
│         └─────────────▶│    Opus        │───▶│    Audio         │  │
│                        │    Decoder     │    │    Playback      │  │
│                        │ (AudioDecoder) │    │  (AudioContext)  │  │
│                        └────────────────┘    └──────────────────┘  │
│                                                                    │
│  ┌────────────────┐    ┌────────────────┐                          │
│  │  Mouse +       │───▶│   Clock Sync   │                          │
│  │  Keyboard      │    │   + Metrics    │                          │
│  │  (Normalized)  │    │                │                          │
│  └────────────────┘    └────────────────┘                          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## Performance Tuning

### Server Optimizations

| Setting | Location | Effect |
|---------|----------|--------|
| Process priority | `main.cpp` | `ABOVE_NORMAL_PRIORITY_CLASS` |
| Encoder thread | `main.cpp` | `THREAD_PRIORITY_TIME_CRITICAL` |
| Audio thread | `main.cpp` | `THREAD_PRIORITY_HIGHEST` |
| D3D11 multithread | `capture.hpp` | Thread-safe context access |

### Recommended Settings

| Scenario | Frame Rate | Expected Bitrate |
|----------|------------|------------------|
| LAN (1Gbps) | 60 FPS | 15-25 Mbps |
| LAN (100Mbps) | 60 FPS | 15-25 Mbps |
| WAN (50Mbps) | 30 FPS | 8-15 Mbps |
| WAN (10Mbps) | 15 FPS | 3-8 Mbps |

## Security Considerations

### Server Hardening

- Credentials hashed with PBKDF2 (600k iterations)
- Rate limiting prevents brute-force attacks
- Session tokens are cryptographically random
- CORS restricted to same-origin
- CSP headers prevent XSS
- Input validation on all API endpoints
- SDP size limited to 64KB

### Input Filtering

- Windows key blocked (prevents accidental OS shortcuts)
- Ctrl+Alt+Delete blocked (security boundary)
- Rate limiting prevents input flooding
- Coordinate clamping to monitor bounds

### Network Security

- WebRTC uses DTLS encryption
- Session tokens required for all streaming operations
- Automatic session expiry on inactivity

## Troubleshooting

### Build Issues

| Problem | Solution |
|---------|----------|
| vcpkg not found | Set `VCPKG_ROOT` environment variable or install to `C:\vcpkg` |
| CMake configuration failed | Ensure Visual Studio 2022 with C++ Desktop workload is installed |
| NVENC not available | NVIDIA GPU with NVENC support is required |

### Connection Issues

| Problem | Solution |
|---------|----------|
| Connection refused | Check firewall allows TCP 6060 and UDP 50000-50020 |
| WebRTC timeout | Ensure UDP is not blocked by NAT/firewall |
| Session expired | Re-authenticate; sessions expire after 24h or 4h inactivity |
| Rate limited | Wait 30 minutes or use different IP |

### Streaming Issues

| Problem | Solution |
|---------|----------|
| Black screen | Wait for keyframe or refresh page |
| Choppy video | Lower frame rate; check network bandwidth |
| No audio | Click "Enable" in audio section; browser requires user gesture |
| High latency | Use wired connection; reduce frame rate; check server CPU usage |

### Client Issues

| Problem | Solution |
|---------|----------|
| Codec unsupported | Use Chrome 94+, Edge 94+, or Firefox 98+ |
| Input not working | Click on video canvas to capture input |
| Keyboard locked | Press Escape twice or exit fullscreen |

## Building Installer

```batch
build_installer.bat
```

Creates:
- `SlipStream-1.0.0-win64.exe` (NSIS installer, if available)
- `SlipStream-1.0.0-win64.zip` (fallback)

## Dependencies

Managed via vcpkg:

| Package | Purpose |
|---------|---------|
| libdatachannel | WebRTC implementation |
| cpp-httplib[openssl] | HTTP server with TLS support |
| nlohmann-json | JSON parsing |
| opus | Audio encoding |
| openssl | Cryptography |
| ffmpeg[nvcodec,avcodec] | Video encoding (NVENC) |

## Known Limitations

- Windows only (server requires WGC API and WASAPI)
- Single client at a time
- NVIDIA GPU required (no software encoding fallback)
- No clipboard sync
- No file transfer
- Hardware cursor only

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial and company use requires a separate license.

See [LICENSE.txt](LICENSE.txt) for full terms.

## Copyright

(c) 2025-2026 Daniel Chrobak. All rights reserved.
