# Host Code Tracker

Use this file to track host-side work status.

Status guide:
- `[ ]` not started
- `[-]` in progress
- `[x]` completed

## Source Files

| Status | File | Notes |
|---|---|---|
| [ ] | `src/host/core/app_support.cpp` | |
| [ ] | `src/host/core/common.cpp` | |
| [ ] | `src/host/host_app.cpp` | |
| [ ] | `src/host/io/input.cpp` | |
| [ ] | `src/host/io/tray.cpp` | |
| [ ] | `src/host/media/audio.cpp` | |
| [ ] | `src/host/media/capture.cpp` | |
| [ ] | `src/host/media/encoder.cpp` | |
| [x] | `src/host/net/port_mapper.cpp` | rewritten implementation promoted to active source |
| [x] | `src/host/net/webrtc.cpp` | rewritten implementation promoted to active source |

## Header Files

| Status | File | Notes |
|---|---|---|
| [ ] | `include/host/core/app_support.hpp` | |
| [ ] | `include/host/core/audio_resampler.hpp` | |
| [ ] | `include/host/core/common.hpp` | |
| [ ] | `include/host/core/d3d_sync.hpp` | |
| [ ] | `include/host/core/logging.hpp` | |
| [ ] | `include/host/core/protocol.hpp` | |
| [ ] | `include/host/core/utils.hpp` | |
| [ ] | `include/host/host_app.hpp` | |
| [ ] | `include/host/io/input.hpp` | |
| [ ] | `include/host/io/tray.hpp` | |
| [ ] | `include/host/media/audio.hpp` | |
| [ ] | `include/host/media/capture.hpp` | |
| [ ] | `include/host/media/encoder.hpp` | |
| [ ] | `include/host/net/port_mapper.hpp` | |
| [ ] | `include/host/net/webrtc.hpp` | |

## Current Notes

- `src/host/net/port_mapper.cpp` now contains the promoted shorter rewrite.
- The temporary `src/host/net/port_mapper.rewrite.tmp.cpp` file has been removed after promotion.
- Inspected `src/host/net/webrtc.cpp` as the best remaining host-net reduction target because it was 1075 lines and contained repeated control dispatch, channel binding, and packet/FEC scaffolding.
- `src/host/net/webrtc.cpp` now contains the promoted shorter rewrite.
- The temporary `src/host/net/webrtc.rewrite.tmp.cpp` file has been removed after promotion.
- Final promoted measurement is 1075 lines down to 843 lines, a net reduction of 232 lines (21.58%).
- Promotion validation completed with editor error checks on `src/host/net/webrtc.cpp`; no diagnostics were reported.
- A CMake build was attempted through the build tool, but no build result was produced because the tool call was skipped.