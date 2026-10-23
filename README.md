# h265sim

A CV25-like camera simulator (`h265sim-server`) that serves low-latency H.265 over TCP, and a client (`h265sim-client`) that tracks every frame's age, drops stale frames and recovers using on-demand IDRs. The design is in [h265-sim-architecture.md](h265-sim-architecture.md).

## Build and test

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
# debug preset: ASan + UBSan
```

Dependencies are system packages, apart from ImGui v1.92.9b and ImPlot v1.0, which FetchContent pins:

- FFmpeg (libavcodec built with libx265)
- standalone Asio
- SDL3
- GTest

## Run

```sh
build/release/h265sim-server config/h265sim.ini
build/release/h265sim-client config/h265sim.ini              # SDL3 window
build/release/h265sim-client config/h265sim.ini --headless   # CSV only
.venv/bin/python tools/analysis/breakdown.py server_frames.csv client_frames.csv --plot stages.png
```

- **Config:** every key in [config/h265sim.ini](config/h265sim.ini) can be overridden on the command line as `section.key=value`, for example `encoder.bitrate_kbps=2000` or `transport.host=10.0.0.5`.
- **Live keys:** keys marked *live* can be changed from the client UI. Server keys are sent as CONFIG_SET, and the server answers with CONFIG_ACK.
- **Flags:**
  - `--seconds=N` stops either binary after N seconds.
  - `--dump=out.h265` makes the server write a raw stream that plays in `ffplay`.
- **Impairment testing:** use `sudo tools/netem/ns.sh` to run the two sides in separate network namespaces under `tc netem`. They still share one clock, so the logged ages are exact.

## Layout

All decisions are pure functions, `step(state, event) -> commands`. Every side effect sits behind a port, and each port has a real implementation and a simulated one.

| Path | What |
|---|---|
| `common/` | wire format, INI config, NAL scanner, log rows, port interfaces; `real.*` holds the real clock, timers, CSV and control TCP |
| `server/core.*` | pacing, ISP delay, encode scheduling, send queue with drop-to-IDR, control handling |
| `client/core.*` | reassembly, frame ages, staleness FSM, decode queue, keyframe-request backoff |
| `*/run.*` | interpreters: run `step` and execute the commands through the ports, in the real binaries and in the sim alike |
| `server/encoder_av.*`, `client/decoder_av.*` | libx265 / libavcodec ports, each on its own worker thread |
| `client/ui.*` | SDL3 + ImGui + ImPlot UI on the main thread |
| `sim/` | deterministic single-threaded simulation: virtual time, link model, fake codec |
| `tests/` | unit tests, a real-codec integration test, and sim scenarios from doc §10 |

On each side, one Asio thread runs every decision. Encoding, decoding and the UI run on their own threads and post completion events back. This keeps the simulator exactly repeatable, and costs about 10–50 µs per handoff, which the `server queue` and `client queue` columns of `breakdown.py` show.
