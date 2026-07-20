# InverterClientImGui ("rte")

Desktop telemetry + control client for the InverterGen5 STM32H723 motor
inverter. Connects to the inverter over the MCP2221A USB-UART bridge
(`/dev/ttyACM0`, 460800 baud), plots live signals, mirrors everything the
device says to disk, and exposes a localhost HTTP API so scripts and
language-model agents can observe and control the inverter without touching
the serial port themselves.

**If you are an LLM agent: do NOT open `/dev/ttyACM0` directly while this app
is running — use the HTTP API below. It is the supported, contention-free
path for reading telemetry, sending shell commands, and flashing firmware.**

## Build & run

```bash
cmake -B build && cmake --build build -j
./build/imgui_api_viewer [serial_port]   # default /dev/ttyACM0
```

First-time dependency for automated flashing (GPIO control of BOOT0/NRST):

```bash
python3 -m venv .venv && .venv/bin/pip install EasyMCP2221 pyserial
```

The app must keep running for the API and the logs to be available. The HTTP
server listens on `http://localhost:18080` (loopback only).

## HTTP API

Base URL: `http://localhost:18080`. All responses are JSON.

### Discover what's here

```bash
curl -s localhost:18080/api/info
```

Returns app name, device port, uptime, `log_dir`, and the endpoint list.

### Read latest telemetry

```bash
curl -s localhost:18080/api/telemetry
```

```json
{"t": 1752881234.567, "rx_hz": 99.7, "suspended": false,
 "signals": {"ph_u_a": 0.012, "vdc_v": 23.91, "enc_angle_deg": 181.4, "...": "..."},
 "strings": {}}
```

`signals` holds the latest value of every float signal the firmware
publishes (~100 Hz). `rx_hz` near 100 means the stream is healthy; `0`
means the device is silent (check the console). `suspended: true` means a
firmware update is in progress and the device is unreachable for a moment.

### Read the device console (shell text output)

```bash
curl -s 'localhost:18080/api/console?lines=100'          # last 100 lines
curl -s 'localhost:18080/api/console?since=4821'         # only newer lines
```

```json
{"lines": [{"seq": 4822, "text": "[SHELL] status ..."}], "latest_seq": 4822}
```

Console lines have monotonically increasing `seq` numbers. For incremental
polling, remember `latest_seq` and pass it as `since` next time. Note: the
in-app buffer is capped; full history is in the console log file (below).

### Send a shell command

```bash
curl -s -X POST --data-binary 'status' localhost:18080/api/command
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"cmd":"help"}' 'localhost:18080/api/command?wait_ms=3000'
```

Sends one line to the device shell and returns the console lines produced
in response (same shape as `/api/console`). `wait_ms` (default 2000, max
30000) bounds how long to wait for the response; a long-running command
(e.g. `cal`, `calpoles`) returns early — poll `/api/console?since=...` for
the rest. Send `help` first to see the device's full command set.

### Flash firmware

Used by `STM32CubeMX/build_flash_uart.sh` automatically; you rarely call it
by hand:

```bash
curl -s -X POST --data-binary @STM32CubeMX.bin localhost:18080/flash   # 202 = queued
curl -s localhost:18080/flash/status            # state/busy/last_error/log tail
```

During a flash, telemetry is suspended (the updater owns the serial port),
then resumed automatically.

## Log files

Written continuously to `<repo>/logs/` (reported as `log_dir` by
`/api/info`), rotated daily:

- `console-YYYY-MM-DD.log` — every device console line, plain text,
  appended live. Grep this for shell command responses, faults, and
  calibration output.
- `telemetry-YYYY-MM-DD.jsonl` — one JSON object per line at ~10 Hz:
  `{"t": <unix_s>, "rx_hz": ..., "signals": {...}, "strings": {...}}`.
  Read/tail this for trends (e.g. `tail -n 600` = last minute).

## Typical LLM debugging workflow

1. `GET /api/telemetry` — is the stream alive? What are the key values?
2. `POST /api/command` with `status`, then `help` — device state and
   available commands.
3. Drive the device with more commands (`cal`, `enc_status`, `foc...`),
   reading each response inline.
4. For anything long-running, poll `GET /api/console?since=...` or read
   the log files under `logs/`.
5. To flash new firmware, run `STM32CubeMX/build_flash_uart.sh` in the
   InverterGen5 repo — it detects this app and flashes through `/flash`
   (override with `INVERTER_CLIENT_FLASH_URL`).

## Repo layout

- `src/main.cpp` — GUI, wiring (serial port, HTTP server, logger)
- `src/telemetry_protocol.*` — serial reader + TLM1 frame parser, `sendLine()`
- `src/http_server.*` — the HTTP API described above
- `src/firmware_updater.*` — UART ROM-bootloader flashing with GPIO sequencing
- `src/telemetry_logger.*` — the `logs/` files described above
- `tools/mcp2221a_gpio.py` — BOOT0/NRST control helper used while flashing
