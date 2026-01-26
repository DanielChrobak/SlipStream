# SlipStream

A high-performance, low-latency remote desktop solution using WebRTC for real-time streaming with AV1 hardware encoding.

## Overview

SlipStream enables secure, low-latency screen sharing over local networks using modern web technologies. The server captures the host display using the Windows Graphics Capture API and encodes it with AV1 hardware acceleration (NVENC, QSV, or AMF). Clients connect via any modern web browser without plugins.

## Features

- **AV1 Hardware Encoding** - Leverages GPU encoders (NVIDIA NVENC, Intel QSV, AMD AMF) with software fallback (libsvtav1)
- **Ultra-Low Latency** - Optimized pipeline for sub-50ms end-to-end latency on local networks
- **WebRTC Transport** - Encrypted peer-to-peer streaming with automatic ICE negotiation
- **System Audio** - Captures and streams system audio via Opus codec
- **Full Input Control** - Complete mouse, keyboard, and scroll input support
- **Multi-Monitor** - Dynamic monitor switching during active sessions
- **Secure Authentication** - Password-based authentication with SHA-256 hashing, rate limiting, and session tokens

## System Requirements

### Host (Server)

- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ Desktop workload
- DirectX 11 compatible GPU
- [vcpkg](https://github.com/microsoft/vcpkg) package manager

### Client

- Modern web browser with WebRTC and AV1 support (Chrome, Firefox, Edge, Safari)
- Hardware video decoding recommended for best performance

## Architecture

```
+-------------------------------------------------------------------+
|                         HOST (Server)                             |
+-------------------------------------------------------------------+
|                                                                   |
|  +-------------+    +-------------+    +-----------------------+  |
|  |   Screen    |--->|     AV1     |--->|       WebRTC          |  |
|  |   Capture   |    |   Encoder   |    |    Data Channel       |  |
|  +-------------+    +-------------+    +-----------+-----------+  |
|                                                    |              |
|  +-------------+    +-------------+                |              |
|  |    Audio    |--->|    Opus     |----------------+              |
|  |   Capture   |    |   Encoder   |                |              |
|  +-------------+    +-------------+                |              |
|                                                    |              |
|  +-------------+    +--------------+               |              |
|  |    Input    |<---|     HTTP     |<--------------+              |
|  |   Handler   |    |   Auth API   |                              |
|  +-------------+    +--------------+                              |
|                                                                   |
+-------------------------------------------------------------------+
                                |
                                |  DTLS/SRTP Encrypted
                                |  Local Network
                                v
+-------------------------------------------------------------------+
|                        CLIENT (Browser)                           |
+-------------------------------------------------------------------+
|                                                                   |
|  +-------------+    +-------------+    +-----------------------+  |
|  |   WebRTC    |--->|    AV1      |--->|       WebGL2          |  |
|  |   Receive   |    |   Decoder   |    |      Renderer         |  |
|  +-------------+    +-------------+    +-----------------------+  |
|                                                                   |
|  +-------------+    +-------------+                               |
|  |    Audio    |<---|    Opus     |                               |
|  |   Playback  |    |   Decoder   |                               |
|  +-------------+    +-------------+                               |
|                                                                   |
|  +-------------+                                                  |
|  |    Input    |---> (sent to server via WebRTC)                  |
|  |   Capture   |                                                  |
|  +-------------+                                                  |
|                                                                   |
+-------------------------------------------------------------------+
```

## Quick Start

### 1. Install Prerequisites

```powershell
# Install vcpkg
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install
```

### 2. Build the Server

```batch
build.bat
```

The build process will:
1. Install dependencies via vcpkg (libdatachannel, FFmpeg, Opus, OpenSSL, etc.)
2. Configure the project with CMake
3. Build the Release executable

Output: `build\bin\Release\SlipStream.exe`

### 3. Run the Server

```batch
run.bat
```

On first run, you'll configure:
- **Username** (3-32 alphanumeric characters, underscores, or hyphens)
- **Password** (8+ characters, must include at least one letter and one number)

Configuration is saved to `auth.json` with the password securely hashed.

### 4. Connect from Client

1. Open a web browser on any device on the same network
2. Navigate to `http://<HOST_IP>:6060`
3. Enter the server URL if prompted
4. Authenticate with your username and password
5. Enable audio if desired, select monitor and frame rate

## Configuration

### auth.json

Credentials are stored with secure password hashing:

```json
{
  "username": "admin",
  "passwordHash": "a]94d5...(64-char SHA-256 hash)",
  "salt": "b7e2c1...(64-char random salt)"
}
```

**Note:** The password is never stored in plaintext. Legacy configurations with plaintext PINs are automatically migrated to the new secure format on first run.

### vcpkg.json

Dependencies are automatically managed by vcpkg:
- libdatachannel (WebRTC)
- cpp-httplib with OpenSSL (HTTP server with TLS support)
- nlohmann-json (JSON parsing)
- opus (Audio codec)
- openssl (Password hashing, TLS/DTLS)
- ffmpeg (AV1 encoding with hardware acceleration)

## Security

### Authentication

- **Password Hashing** - Passwords are hashed using SHA-256 with unique random salts
- **Constant-Time Comparison** - Password verification uses constant-time comparison to prevent timing attacks
- **Rate Limiting** - 5 failed attempts triggers a 30-minute lockout per IP address
- **Session Tokens** - Authenticated sessions use 64-character cryptographic tokens
- **Session Expiry** - Sessions expire after 24 hours or 4 hours of inactivity

### Transport Security

- All WebRTC streams are encrypted with DTLS/SRTP
- HTTP authentication endpoints use secure token-based flow
- Sessions are isolated per connection

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/auth` | POST | Authenticate with username/password, returns session token |
| `/api/session` | GET | Validate existing session token |
| `/api/logout` | POST | Invalidate session token |
| `/api/offer` | POST | WebRTC signaling (requires valid session token) |

## Web Client

The server hosts these files on port 6060:

| File | Description |
|------|-------------|
| `index.html` | Main application shell with auth modals |
| `styles.css` | UI styling |
| `js/network.js` | WebRTC connection and HTTP authentication |
| `js/renderer.js` | WebGL2 video rendering |
| `js/input.js` | Mouse, keyboard handling |
| `js/media.js` | Video and audio decoding |
| `js/state.js` | Application state management |
| `js/ui.js` | Settings panel and controls |

## Controls

### Mouse & Keyboard

- **All mouse actions** - Click, drag, scroll passed through to host
- **All keyboard input** - Passed through to host (except OS shortcuts)
- **Right-click** - Context menu on host
- **Scroll wheel** - Vertical and horizontal scrolling

### Settings Panel

- **Edge trigger** - Hover/click the right edge to open settings
- **Disconnect** - End session and return to connection screen
- **Fullscreen** - Toggle fullscreen mode (with keyboard lock for Escape key)
- **Audio** - Enable/disable system audio streaming
- **Monitor** - Switch between available displays
- **Frame Rate** - Adjust streaming FPS (15/30/60/Host/Client native)

## Performance Tuning

### Encoder Selection Priority

1. **NVIDIA NVENC** - Lowest latency, best quality (requires NVIDIA GPU)
2. **libsvtav1** - Fast software fallback (universal)

### Optimizing Latency

- Use wired network connections when possible
- Select frame rate matching host display refresh rate
- Ensure GPU hardware encoding is active (check console output)
- Client browser should use hardware video decoding

### GPU Synchronization

The capture pipeline uses fence-based or query-based GPU synchronization:
- **Fence mode** - Preferred on newer drivers, lower latency
- **Query mode** - Fallback for older systems

## Troubleshooting

### Connection Issues

**Cannot connect to server**
- Verify server is running (console shows status)
- Check firewall allows port 6060 (TCP) and 50000-50020 (UDP)
- Ensure devices are on the same network

**Authentication failed**
- Check username and password are correct
- If rate limited, wait 30 minutes or restart server
- Verify auth.json exists and is valid

**Session expired**
- Re-authenticate with your credentials
- Sessions expire after 24 hours or 4 hours of inactivity

### Build Issues

**vcpkg not found**
- Set `VCPKG_ROOT` environment variable
- Or install vcpkg to `C:\vcpkg`

**OpenSSL errors**
- Ensure cpp-httplib is built with OpenSSL support
- Check vcpkg.json includes the OpenSSL feature

**Encoder initialization failed**
- Update GPU drivers
- Verify DirectX 11 support
- Software fallback (libsvtav1) will be used if hardware unavailable

### Performance Issues

**High latency**
- Check network connection quality
- Reduce frame rate in settings
- Verify hardware encoding is active (check console for "Encoder: av1_nvenc")

**Choppy video**
- Lower frame rate
- Check client CPU/GPU usage
- Ensure hardware video decoding is enabled in browser

**Texture conflicts**
- Console shows texture conflict count
- Usually resolves automatically, may indicate GPU overload

## Building Installer

```batch
build_installer.bat
```

Creates either:
- NSIS installer (.exe) if NSIS is installed
- Portable ZIP package otherwise

## License

Business Source License (Personal-Online / No-Company) v1.1

See [LICENSE.txt](LICENSE.txt) for full terms.

**Summary:** Free for personal use. Commercial/company use requires a separate license.

## Copyright

© 2025-2026 Daniel Chrobak
