# SlipStream

Low-latency remote desktop streaming over WebRTC with hardware-first video encode and decode paths.

**Installer workflow, command-line arguments, client UI features, D3D11 sync, and configuration/auth setup are now documented.**
## Overview

SlipStream captures the Windows desktop with the Windows Graphics Capture API and encodes frames in real time with FFmpeg-backed encoder paths. It prefers hardware encoders on NVIDIA, Intel, and AMD GPUs, but can fall back to software encoders when the selected codec is not available in hardware. Video, system audio, control traffic, input, and microphone audio are carried over WebRTC data channels with DTLS transport from the browser/libdatachannel stack. The browser client renders decoded frames with WebGL2, uses WebCodecs for video and audio decode, supports software decode fallback when hardware decode is unavailable, and can send microphone audio upstream with WebCodecs AudioEncoder. The server runs as an elevated Windows tray application and can also be installed with a scheduled task for boot-time startup.

## Architecture

```
+------------------------------------------------------------------------------+
|                              SERVER (Windows)                                |
+------------------------------------------------------------------------------+
|  Screen Capture         Encoder             WebRTC              Audio        |
|  +--------------+    +--------------+    +----------------+    +----------+  |
|  | WGC API      |--->| HW + SW      |--->| Data Channel   |<-->| WASAPI   |  |
|  | D3D11 Fence  |    | FFmpeg Enc   |    | libdatachannel |    | Opus     |  |
|  | 6-tex pool   |    | AV1/H265/264 |    | 5 channels     |    | 96 kbps  |  |
|  +--------------+    +--------------+    +----------------+    +----------+  |
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
                          WebRTC (UDP/TCP 50000-50127)
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

## Requirements

### Server

| Component | Requirement |
|-----------|-------------|
| OS | Windows 10/11 64-bit |
| GPU | NVIDIA, Intel, or AMD GPU recommended for hardware encode; software encode fallback is available |
| IDE | Visual Studio 2022 with C++20 Desktop workload |
| Package Manager | [vcpkg](https://github.com/microsoft/vcpkg) |
| Microphone Output | VB-Cable optional (`CABLE Input` preferred), default render fallback supported |

### Client

| Component | Requirement |
|-----------|-------------|
| Browser | Modern browser with WebRTC DataChannel support; Chromium-based browsers are recommended for best WebCodecs coverage |
| APIs | WebRTC, WebCodecs (VideoDecoder, AudioDecoder, AudioEncoder, AudioData), WebGL2, AudioWorklet, Clipboard API |

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
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target SlipStream
```

Output: `build\bin\Release\SlipStream.exe`

### 3. Run

```batch
build\bin\Release\SlipStream.exe
```

Optional debug logging:

```batch
build\bin\Release\SlipStream.exe --debug
```

On first run:
- Prompts for username (3-32 alphanumeric characters, underscores, hyphens)
- Prompts for password (8-128 characters with at least one letter and one digit)
- Generates self-signed SSL certificate (`server.crt`, `server.key`) in `%APPDATA%\SlipStream`
- Saves hashed credentials to `%APPDATA%\SlipStream\auth.json`
- Generates JWT secret to `%APPDATA%\SlipStream\jwt_secret.dat`
- Creates log file at `%APPDATA%\SlipStream\slipstream.log`
- Requests elevation on launch for capture/input features (`requireAdministrator` manifest)
- Hides console to system tray

### 4. Connect

Open browser to `https://<HOST_IP>:443` and log in with your credentials.

At startup, SlipStream prints:
- `Local` URL (`https://localhost:443`)
- All detected non-loopback LAN IPv4 `Network` URLs

> **Note:** Self-signed certificate will trigger a browser warning. Click through to proceed.

## Network Configuration

| Port | Protocol | Purpose |
|------|----------|---------|
| 443 | TCP | HTTPS server and WebRTC signaling |
| 50000-50127 | UDP/TCP | WebRTC data transport, ICE checks, and ICE-TCP fallback |
| 1900 | UDP (outbound) | UPnP SSDP discovery for automatic router mapping |
| 5351 | UDP (outbound) | PCP/NAT-PMP mapping requests and lease renewals to the default gateway |

### ICE Configuration

| Parameter | Value |
|-----------|-------|
| ICE TCP | Enabled on host as a fallback; UDP remains preferred |
| ICE Candidate Pool | 4 |
| Bundle Policy | max-bundle |
| RTCP Mux Policy | require |

### STUN Servers

- `stun:stun.l.google.com:19302`
- `stun:stun1.l.google.com:19302`
- `stun:stun2.l.google.com:19302`
- `stun:stun3.l.google.com:19302`
- `stun:stun4.l.google.com:19302`
- `stun:stun.cloudflare.com:3478`
- `stun:stun.services.mozilla.com:3478`
- `stun:global.stun.twilio.com:3478`

### NAT Traversal Notes

SlipStream does not bundle a TURN server. For the best chance of direct WAN connectivity without TURN:

- Prefer forwarding `UDP 50000-50127` to the host.
- If either side is on a network that filters UDP aggressively, also forward `TCP 50000-50127` so ICE-TCP candidates can be reached.
- SlipStream now attempts consumer-router auto-mapping on the host with `UPnP`, then `PCP`, then `NAT-PMP`.
- Automatic mapping targets `TCP 443` for signaling, `UDP 50000-50127` for WebRTC, and `TCP 50000-50127` when ICE-TCP fallback is enabled.
- Automatic mapping also sends outbound router-control traffic on `UDP 1900` for UPnP discovery and `UDP 5351` for PCP/NAT-PMP requests.
- If you do not want SlipStream to create or renew router mappings automatically, set `SLIPSTREAM_ENABLE_PORT_MAPPING=0` before launch.
- To change the requested router-mapping lease, set `SLIPSTREAM_PORTMAP_LEASE_SECONDS` to a value between `120` and `86400` before launch.
- Manual forwarding is still useful when the router does not support these protocols, when the router blocks them by policy, or when you want a permanent mapping instead of a renewable lease.

### Firewall

```powershell
netsh advfirewall firewall add rule name="SlipStream HTTPS" dir=in action=allow protocol=tcp localport=443
netsh advfirewall firewall add rule name="SlipStream WebRTC UDP" dir=in action=allow protocol=udp localport=50000-50127
netsh advfirewall firewall add rule name="SlipStream WebRTC TCP" dir=in action=allow protocol=tcp localport=50000-50127
```

If outbound firewalling is enabled on the host, also allow `UDP 1900` to the local network for UPnP discovery and `UDP 5351` to the default gateway for PCP/NAT-PMP.

## Security

### Password Storage

| Parameter | Value |
|-----------|-------|
| Algorithm | PBKDF2-HMAC-SHA256 |
| Iterations | 600,000 |
| Salt | 16 bytes (random) |
| Key Length | 32 bytes |

Passwords are never stored in plain text. Only the salt and derived hash are saved in `%APPDATA%\SlipStream\auth.json`.

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
- X.509 v3 with SHA-256 signature
- 10-year validity (3,650 days)
- Subject: `C=US, O=SlipStream, CN=localhost`
- Subject Alt Names include `localhost`, `127.0.0.1`, host DNS name, and detected LAN IPv4 addresses
- Extensions: `CA:FALSE`, `keyUsage: digitalSignature, keyEncipherment`, `extKeyUsage: serverAuth`

If you previously generated certificates, delete `%APPDATA%\SlipStream\server.crt` and `%APPDATA%\SlipStream\server.key` to regenerate with updated SANs.

To use custom certificates, replace `%APPDATA%\SlipStream\server.crt` and `%APPDATA%\SlipStream\server.key` before starting.

### Security Headers

| Header | Value |
|--------|-------|
| X-Content-Type-Options | nosniff |
| X-Frame-Options | DENY |
| Referrer-Policy | no-referrer |

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

If the primary GPU's encoder path fails, SlipStream probes other vendor-specific hardware encoders and then software encoders for the active codec.

### Encoding

| Setting | H.264 | H.265 | AV1 |
|---------|-------|-------|-----|
| NVIDIA | h264_nvenc | hevc_nvenc | av1_nvenc |
| Intel | h264_qsv | hevc_qsv | av1_qsv |
| AMD | h264_amf | hevc_amf | av1_amf |

Software fallback encoders are also supported when available:

| Codec | Software Fallback Order |
|------|--------------------------|
| H.264 | libx264 |
| H.265 | libx265 |
| AV1 | libsvtav1, libaom-av1, librav1e |

#### NVIDIA NVENC Configuration

| Setting | Value |
|---------|-------|
| Preset | P2 |
| Tune | Ultra-low latency |
| Rate Control | VBR |
| CQ Level | Dynamic by codec / resolution / FPS (base 25 H.264, 28 H.265, 31 AV1) |
| Zero Latency | Enabled |
| Lookahead | Disabled |
| Surfaces | 3 |

#### Intel QSV Configuration

| Setting | Value |
|---------|-------|
| Preset | veryfast |
| Look Ahead | Disabled |
| Async Depth | 1 |
| Low Power | Enabled |
| Global Quality | Dynamic by codec / resolution / FPS (base 25 H.264, 28 H.265, 31 AV1) |
| Forced IDR | Enabled |

#### AMD AMF Configuration

| Setting | Value |
|---------|-------|
| Usage | Ultra-low latency |
| Quality | Balanced |
| Rate Control | VBR latency |
| Header Insertion | GOP |
| QP | Dynamic by codec / resolution / FPS (base 25 H.264, 28 H.265, 31 AV1) |

**Bitrate Formula:** bitrate is codec-aware and uses an effective-FPS curve.

- AV1: `max(6,000,000, 0.112 × width × height × effectiveFps)`
- H.265: `max(6,000,000, 0.138 × width × height × effectiveFps)`
- H.264: `max(6,000,000, 0.165 × width × height × effectiveFps)`

`effectiveFps = fps` for `fps <= 60`, `60 + 2/3 × (fps - 60)` for `61-90`, and `80 + floor((fps - 90) / 3)` above 90 FPS.

For all codecs, `maxrate` is clamped to at least the target bitrate and otherwise set to 115% of target bitrate.

### Transport

| Parameter | Value |
|-----------|-------|
| Chunk Size | 1,400 bytes max |
Output: `build\bin\Release\SlipStream.exe`
| Data Payload | Up to 1,345 bytes per video packet |
### 3. Installer Build

To create a Windows installer (with scheduled task, firewall rules, and shortcuts):

```batch
build_installer_release.bat
```

Creates `build\SlipStream-1.0.0-Release-Setup.exe` using Inno Setup. See `SlipStream.iss` for installer script details.

For debug builds:

```batch
build_installer_debug.bat
```

Creates `build\SlipStream-1.0.0-Debug-Setup.exe` (launches with `--debug`).

| Video Buffer | 256 KB threshold |
| Max Queue | 2,048 packets (trimmed to 512 on overflow) |
| Congestion Threshold | 256 queued packets or 128 KB buffered in the video data channel |
| Delivery | Unordered, maxRetransmits=0 |
```batch
build\bin\Release\SlipStream.exe [--debug] [--trace] [--force-software]
```

**Command-line arguments:**
- `--debug`: Enables debug logging
- `--trace`: Enables verbose trace output
- `--force-software`: Forces software encoding for all codecs

### Forward Error Correction (FEC)
### Video Packet Header (55 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | timestamp | Capture timestamp (microseconds) |
| 24 | 8 | enqueueTimestamp | Timestamp when packetization begins |
| 32 | 4 | encodeTimeUs | Encode duration (microseconds) |
| 36 | 4 | frameId | Frame sequence number |
| 40 | 4 | frameSize | Encoded frame size in bytes |
| 44 | 2 | chunkIndex | Chunk index (data: chunk number, FEC: group number) |
| 46 | 2 | totalChunks | Number of data chunks |
| 48 | 2 | chunkBytes | Payload bytes in this packet |
| 50 | 2 | dataChunkSize | Nominal data-chunk size |
| 52 | 1 | frameType | 1=keyframe, 0=delta |
| 53 | 1 | packetType | 0=data, 1=FEC parity |
| 54 | 1 | fecGroupSize | Effective FEC group size for this frame |

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
| Max Queue | 6 packets (WebRTC send queue), 4 packets capture queue |
| Delivery | Unordered, maxRetransmits=0 |
| FEC | XOR parity packet every 10 audio packets |

### Audio Packet Header (24 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | 0x41554449 ("AUDI") |
| 4 | 8 | timestamp | Capture timestamp |
| 12 | 4 | packetId | Monotonic packet sequence |
| 16 | 2 | samples | Sample count (data packets only) |
| 18 | 2 | dataLength | Payload size |
| 20 | 1 | packetType | 0=data, 1=FEC parity |
| 21 | 1 | fecGroupSize | FEC group size (currently 10) |
| 22 | 2 | reserved | Reserved (0) |

### Client Playback

| Component | Detail |
|-----------|--------|
| Decoder | AudioDecoder (WebCodecs) |
| Processor | AudioWorklet with ring buffer |
| Ring Buffer Capacity | 24,000 samples (500 ms) |
| Prebuffer Threshold | 1,920 samples (40 ms) |
| Target Buffer | 2,880 samples (60 ms) |
| Max Buffer | 5,760 samples (120 ms) |
| Underrun Adaptation | After 4 consecutive underruns, increases target by 480 samples (10 ms) |
| Health Adjustment | Every 2 seconds, reduces target by 240 samples (5 ms) if buffer > 90 ms |

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
| Delivery | Unordered, maxRetransmits=0 |
| Max Queue | 20 packets |
| FEC | XOR parity packet every 10 mic packets |

### Microphone Packet Header (24 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | 0x4D494344 ("MICD") |
| 4 | 8 | timestamp | Capture timestamp |
| 12 | 4 | packetId | Monotonic packet sequence |
| 16 | 2 | samples | Sample count (data packets only) |
| 18 | 2 | dataLength | Payload size |
| 20 | 1 | packetType | 0=data, 1=FEC parity |
| 21 | 1 | fecGroupSize | FEC group size (currently 10) |
| 22 | 2 | reserved | Reserved (0) |

### Server Playback

| Component | Detail |
|-----------|--------|
| Output Device | VB-Cable Input preferred (`CABLE Input`) |
| Decoder | Opus (libopus) |
| Resampling | Speex polyphase resampler (quality 4) if device rate differs |
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
| 9 | 1 | modifiers (currently sent by client, ignored by server) |

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

- Ctrl+Alt+Delete (blocked server-side)
- Windows key and ContextMenu (not captured by client without fullscreen keyboard lock)

## Control Messages

| Message | Magic | Size | Description |
|---------|-------|------|-------------|
| PING | 0x504E4750 | 16/24 | Clock synchronization |
| HOST_INFO | 0x484F5354 | 7 | Host display refresh rate plus host flags |
| FPS_SET | 0x46505343 | 7 | Set target frame rate |
| FPS_ACK | 0x46505341 | 7 | Frame rate acknowledgment |
| CODEC_SET | 0x434F4443 | 5 | Set video codec |
| CODEC_ACK | 0x434F4441 | 5 | Codec acknowledgment |
| CODEC_CAPS | 0x434F4350 | 5 | Server codec capabilities bitmask |
| SOFTWARE_ENCODE | 0x4E455753 | 5 | Enable or disable software encoding on the host |
| ENCODER_INFO | 0x49434E45 | Variable | Active codec, host encode flags, and encoder name |
| REQUEST_KEY | 0x4B455952 | 4 | Request keyframe |
| MONITOR_LIST | 0x4D4F4E4C | Variable | Monitor enumeration |
| MONITOR_SET | 0x4D4F4E53 | 5 | Switch monitor |
| AUDIO_ENABLE | 0x41554445 | 5 | Enable/disable audio streaming |
| MIC_ENABLE | 0x4D494345 | 5 | Enable/disable mic streaming |
| CURSOR_CAPTURE | 0x43555243 | 5 | Toggle cursor capture in video |
| CLIPBOARD_GET | 0x434C4754 | 4 | Request clipboard contents |
| CLIPBOARD_DATA | 0x434C4950 | 8+N | Clipboard text transfer |
| VERSION | 0x56455253 | Variable | Host version string |
| KICKED | 0x4B49434B | 4 | Client disconnected by server |

### Codec Capabilities (CODEC_CAPS)

The server sends a bitmask indicating supported codecs:
- Bit 0 (0x01): AV1 supported
- Bit 1 (0x02): H.265 supported
- Bit 2 (0x04): H.264 supported

The client uses this to enable/disable codec options in the UI.

### Host Flags

`HOST_INFO` and `ENCODER_INFO` expose host-side encode state bits:

- Bit 0 (`0x01`): software encoding currently active
- Bit 1 (`0x02`): software encoding forced because the selected codec has no hardware encoder available

## WebRTC Data Channels

| Channel | Ordered | MaxRetransmits | Purpose |
|---------|---------|----------------|---------|
| control | Yes | 3 | Commands, ping/pong, monitor list |
| video | No | 0 | Video frame chunks |
| audio | No | 0 | Audio packets |
| input | Yes | 3 | Mouse/keyboard events |
| mic | No | 0 | Microphone audio packets |

## Client Modules

| File | Purpose |
|------|---------|
| `constants.js` | Protocol message types, codec definitions, data channel configs, tuning constants |
| `state.js` | Shared state, logging, codec detection, clock sync, metrics collection |
| `network.js` | WebRTC signaling, data channel management, FEC handling, reconnection logic |
| `renderer.js` | WebGL2 rendering with aspect ratio letterboxing |
| `media.js` | VideoDecoder, AudioDecoder, keyframe retry, and AudioWorklet plumbing |
| `input.js` | Mouse/keyboard capture, RAF batching, pointer lock |
| `ui.js` | Settings panel, fullscreen, software encode/decode toggles, tabbed mode, stats overlay |
| `mic.js` | Microphone capture, AudioEncoder, packet transmission |
| `auth.js` | Authentication flow, session validation, login/logout UI |
| `protocol.js` | Message construction, keyframe throttling, FEC recovery, adaptive quality |
| `audio-worklet.js` | Audio output AudioWorklet processor with adaptive ring buffer |
| `mic-worklet.js` | Microphone input AudioWorklet processor (10 ms frame buffering) |

### Clock Synchronization

| Parameter | Value |
|-----------|-------|
| Ping interval | 200 ms |
| Sample count | 8 |
| Offset calculation | Median filter |
| RTT estimation | Median of samples |
| RTT validity range | 0–5,000,000 µs |
| Minimum samples for valid sync | 2 |

### Reconnection

| Parameter | Value |
|-----------|-------|
| Max reconnection attempts | 3 |
| Backoff | `min(1000 × attempt, 5000)` ms |
| Server busy (503) retries | 5 |
| Server busy backoff | `min(1000 × (retry + 1), 5000)` ms |
| First-frame watchdog | Warning logged after 3,000 ms |
| After max failures | Clears session, returns to login screen |

### Adaptive Quality

Triggered by sustained decode pressure (≥ 12 consecutive high-queue events, cooldown 6,000 ms between adjustments):

1. **FPS reduction** — If FPS > 30, reduce to 30 (or 24 if currently ≤ 45)
2. **Codec fallback** — AV1 → H.265 → H.264 (one-time per session)

### Frame Handling

| Parameter | Value |
|-----------|-------|
| Max in-flight frames | 64 (minimum), scaled by `ceil(fps × 900 / 1000) + 12` |
| Frame timeout | 900 ms |
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
- **Software Encode** - Prefer software encoding on the host when available; forced on when hardware encode is unavailable for the active codec
- **Software Decode** - Prefer software decoding on the client; forced on when the browser lacks hardware decode for the active codec
- **Tabbed Mode** - Monitor tabs at top of screen
- **Debug Stats** - Real-time performance overlay
- **Relative Mouse** - Pointer lock mode for gaming
- **Clipboard Sync** - Enable Ctrl+C/V synchronization

### Tabbed Mode

When enabled, displays a tab strip showing all monitors with:
- Monitor name (EDID friendly name or user rename)
- Primary indicator (star icon)
- Click to switch monitors
- Double-click active tab to rename monitor label (saved in browser localStorage)

### Debug Stats Overlay

Real-time metrics display organized into sections:

## UI Features

| Section | Metrics |
|---------|---------|

## Server Components

| Main | Default | Tray message pump, main loop |
| Encoder | Time Critical | Frame encoding and sending |
| Audio Capture | Time Critical | WASAPI loopback capture and Opus encoding |
| Audio Send | Highest | Pops encoded audio and sends over WebRTC |
| Mic Playback | Time Critical | Opus decode and WASAPI render to VB-Cable |
| HTTPS Server | Default | cpp-httplib HTTPS listener |
| Cursor | Below Normal | Cursor shape polling |
| Wiggle | Default | Mouse cursor initialization after monitor switch |

Process priority class: `ABOVE_NORMAL_PRIORITY_CLASS`

### System Tray

| Feature | Detail |
|---------|---------|
| Behavior | Console window hides to system tray on startup and when minimized |
| Context Menu | Right-click tray icon: **Open SlipStream** (restore console) or **Exit SlipStream** (shutdown) |
| Console Close | Disabled (grayed out) to prevent accidental termination |
| Icon | Loaded from executable resource, fallback to default system icon |

### Server Header Files

| File | Purpose |
|------|---------|
| `host_app.hpp` | Main application class, session orchestration |
| `common.hpp` | Types, auth utilities, monitor enumeration, SSL generation |
| `app_support.hpp` | App setup, credential prompts, auth helpers |
| `logging.hpp` | Logging infrastructure |
| `protocol.hpp` | Protocol message definitions and magic values |
| `utils.hpp` | Utility functions |
| `d3d_sync.hpp` | D3D11 fence synchronization (ID3D11Fence) |
| `audio_resampler.hpp` | Speex-based audio resampler |
| `capture.hpp` | Screen capture with WGC, texture pool, frame slot |
| `encoder.hpp` | Video encoding via FFmpeg (hardware-first with software fallback) |
| `webrtc.hpp` | WebRTC server, data channels, packet headers |
| `audio.hpp` | WASAPI audio capture + Opus encoding, mic playback |
| `input.hpp` | Input handling, keyboard/mouse injection, clipboard |
| `tray.hpp` | System tray icon and context menu |

## Dependencies

Managed via vcpkg (`vcpkg.json`):

| Package | Purpose |
|---------|---------|
| libdatachannel | WebRTC implementation |
| cpp-httplib[openssl] | HTTPS server |
| nlohmann-json | JSON parsing |
| opus | Audio encoding/decoding |
| openssl | Cryptography, SSL/TLS |
| ffmpeg[nvcodec,avcodec] | Hardware and software video encoding support |
| jwt-cpp | JWT token handling |
| speexdsp | Audio resampling |

The build also links FFmpeg's `avutil` and `swscale` components for frame handling and software encode paths.

## Building

### Standard Build

```batch
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target SlipStream
```

### Standard Run

```batch
build\bin\Release\SlipStream.exe
```

The executable is built with a `requireAdministrator` manifest, so Windows will prompt for elevation when it starts.

### Installer Build

Requires [Inno Setup 6.3+](https://jrsoftware.org/isdl.php).

```batch
build_installer_release.bat
```

Creates `build\SlipStream-1.0.0-Release-Setup.exe`.

The installer offers:
- Windows Firewall exception (program-based, inbound + outbound)
- Startup scheduled task (runs as SYSTEM before login)
- Desktop shortcut

On uninstall:
- Removes firewall rules and scheduled task
- Deletes `%APPDATA%\SlipStream` data files (auth, certs, JWT secret)

### Installer Build (Debug)

```batch
build_installer_debug.bat
```

Creates `build\SlipStream-1.0.0-Debug-Setup.exe` and configures shortcuts/startup launch with:

```batch
SlipStream.exe --debug
```

## File Structure

```
SlipStream/
├── include/
│   └── host/
│       ├── host_app.hpp          # Main application class
│       ├── core/
│       │   ├── common.hpp        # Common includes, types, auth utilities
│       │   ├── app_support.hpp   # App setup/auth helpers
│       │   ├── logging.hpp       # Logging infrastructure
│       │   ├── protocol.hpp      # Protocol message definitions
│       │   ├── utils.hpp         # Utility functions
│       │   ├── audio_resampler.hpp # Speex audio resampler
│       │   └── d3d_sync.hpp      # D3D11 fence synchronization
│       ├── io/
│       │   ├── tray.hpp          # System tray declarations
│       │   └── input.hpp         # Input handling + clipboard
│       ├── media/
│       │   ├── capture.hpp       # Screen capture with WGC
│       │   ├── encoder.hpp       # Video encoding (hardware-first with software fallback)
│       │   └── audio.hpp         # WASAPI audio capture + Opus + mic playback
│       └── net/
│           └── webrtc.hpp        # WebRTC server declarations
├── src/
│   ├── main.cpp                  # Application entry point
│   ├── version.rc                # Windows version resource
│   └── host/
│       ├── host_app.cpp          # Application orchestration
│       ├── core/
│       │   ├── common.cpp        # Auth/SSL/shared runtime implementations
│       │   └── app_support.cpp   # App setup/auth/helpers
│       ├── io/
│       │   ├── tray.cpp          # System tray integration
│       │   └── input.cpp         # Input injection/clipboard
│       ├── media/
│       │   ├── capture.cpp       # Screen capture pipeline
│       │   ├── encoder.cpp       # Video encoder pipeline
│       │   └── audio.cpp         # System audio capture + mic playback
│       └── net/
│           └── webrtc.cpp        # WebRTC server implementation
├── client/
│   ├── index.html                # Web client HTML
│   ├── styles.css                # Stylesheet
│   └── js/
│       ├── constants.js          # Protocol constants, codec definitions
│       ├── state.js              # Shared state and metrics
│       ├── network.js            # WebRTC signaling, protocol, and reconnection
│       ├── renderer.js           # WebGL2 rendering
│       ├── media.js              # Video/audio decoding and playback pipeline
│       ├── input.js              # Input capture
│       ├── ui.js                 # UI controls
│       ├── mic.js                # Microphone capture and encoding
│       ├── auth.js               # Authentication flow
│       ├── protocol.js           # Message construction, FEC recovery
│       ├── audio-worklet.js      # Audio output worklet processor
│       └── mic-worklet.js        # Microphone input worklet processor
├── vcpkg.json                    # Dependencies
├── CMakeLists.txt                # Build configuration
├── build_installer_release.bat   # Release installer builder
├── build_installer_debug.bat     # Debug installer builder (--debug launch)
├── SlipStream.iss                # Inno Setup installer script
├── LICENSE.md                    # Styled license text
└── Installer-LICENSE.txt         # Installer license text
```

## Microphone Setup (VB-Cable)

To enable client microphone audio on the server:

1. Install [VB-Cable Virtual Audio Device](https://vb-audio.com/Cable/)
2. The server attempts to detect `CABLE Input` as the output device
3. Configure your target application to use `CABLE Output` as its microphone input

If VB-Cable is not installed, SlipStream falls back to the default audio render endpoint.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| vcpkg not found | Set `VCPKG_ROOT` environment variable or install to `C:\vcpkg` |
| No hardware encoder | SlipStream can fall back to software encode for supported codecs; expect higher CPU usage and lower headroom |
| Connection refused | Check firewall for TCP 443 and UDP/TCP 50000-50127 |
| Black screen | Wait for keyframe; verify codec compatibility and channel readiness |
| No audio | Click "Enable" in settings panel after connecting |
| Mic not working | Ensure browser has microphone permission and supports `AudioEncoder` plus `AudioData` |
| Input not working | Click the video canvas to capture input focus |
| Certificate warning | Expected with self-signed certificate, click proceed |
| Kicked message | Another client connected (single client only) |
| Monitor switch issues | Frames from previous monitor generations are automatically discarded |
| Codec grayed out | Either the client browser or the host encoder probe does not support that codec |
| Software encode forced | The selected codec has no hardware encoder path on the host |
| Software decode forced | The browser has no hardware decoder for the selected codec |

## Known Limitations

- Windows server only
- Single client connection (new client kicks existing)
- No custom cursor bitmap sync (standard cursor type sync only)
- WebGL2 and WebCodecs are required on the client
- Client microphone uplink requires browser support for `AudioEncoder` and `AudioData`
- Software encode and decode fallbacks work, but they increase CPU load and can reduce latency headroom

## Data Storage

All persistent data is stored in `%APPDATA%\SlipStream\`:

| File | Contents |
|------|---------|
| `auth.json` | Username, password hash (PBKDF2), salt |
| `jwt_secret.dat` | 32-byte HS256 secret (64 hex characters) |
| `server.crt` | Self-signed X.509 certificate |
| `server.key` | RSA 2048-bit private key |
| `slipstream.log` | Application log file |

## Codec Support Detection

### Server-Side

The server probes for encoder support on startup:
1. Detects primary GPU vendor (NVIDIA/Intel/AMD)
2. Tests each codec (AV1, H.265, H.264) with the vendor's encoder
3. Falls back to other hardware vendors if the detected vendor path is unavailable
4. Falls back to software encoders when no hardware path is available for a codec
5. Sends codec capabilities, host flags, and active encoder information to the client

### Client-Side

The client automatically detects codec support on connection:
1. Tests hardware acceleration for each codec (AV1, H.265, H.264)
2. Falls back to software decoding when hardware decode is unavailable or user-selected
3. Intersects browser support with server codec capabilities
4. Forces software decode when the selected codec has no hardware decoder in the browser
5. Surfaces codec availability plus software encode/decode state in the UI

## License

Business Source License (Personal-Online / No-Company) v1.1

Personal use permitted. Commercial use requires separate license.

- Styled/readable version: [LICENSE.md](LICENSE.md)
- Installer-rendered copy: [Installer-LICENSE.txt](Installer-LICENSE.txt)

## Copyright

(c) 2026 Daniel Chrobak. All rights reserved.
