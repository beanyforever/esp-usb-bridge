# ESP-Prog-2 high-throughput UART bridge (Stage 1)

A small fork of `esp-usb-bridge` that makes the ESP-Prog-2 serial bridge capable of a
**lossless, sustained raw UART read** far above the stock ceiling.

- **Stock firmware:** a full raw dump only survives at **115200**; higher rates drop bytes
  partway through (probabilistic buffer overflow, zero recovery), corrupting an un-framed dump.
- **This firmware:** validated **byte-for-byte lossless from 115200 up to 4,000,000 baud
  (400 KB/s)** — 4.4× the original 90 KB/s goal. See [Validation](#validation).

Only the low-risk **Stage 1** of the original plan was implemented (buffer sizing + removing
artificial delays). It proved more than sufficient; Stage 2 and Stage 3 were **not needed**
(see [What was not done](#what-was-not-done-and-why)).

---

## What changed

Four changes, plus one correctness fix. All are commented in-source with rationale.

| File | Change | Why |
|---|---|---|
| `main/public_include/tusb_config.h` | `CFG_TUD_CDC_RX/TX_BUFSIZE` **64 → 512** | Lets `usb_sender_task` hand TinyUSB a full block per blocking wait instead of one 64-byte wire packet at a time (~8× the drain-per-round-trip). The FS wire max-packet stays 64 (`CFG_TUD_CDC_EP_BUFSIZE` keeps its TinyUSB FS default); only the FIFO depth grows. |
| `main/serial_bridge.c` | `USB_SEND_RINGBUFFER_SIZE` **2 KB → 32 KB** | ~350 ms of stall margin at 90 KB/s (was ~22 ms), so ordinary host/USB scheduler hiccups no longer overflow the ring. |
| `components/serial_handler/serial_handler.c` | driver RX ring **4 KB → 16 KB** (new `SLAVE_UART_RX_RINGBUF_SIZE`) | The real stall buffer for target→PC data; heap-resident inside the UART driver. |
| `components/serial_handler/serial_handler.c` | `queue_size` **20 → 40** | More UART-event headroom before `UART_BUFFER_FULL`. |
| `components/serial_handler/serial_handler.c` | **drain the whole driver ring per event** (chunk-sized reads, 0 timeout) | A single event can represent a large backlog from a stall; draining it in one pass (instead of one chunk per queued event) is what lets the 16 KB ring recover after a stall. |
| `components/serial_handler/serial_handler.c` | **removed** the per-loop `vTaskDelay(10 ms)` in `uart_event_task` | The task already blocks on `xQueueReceive(portMAX_DELAY)`, so the nap did nothing but cap drain throughput. |

### The one non-obvious fix (avoids a stack overflow)

`SLAVE_UART_BUF_SIZE` was used in **two** places: the driver's heap RX ring (`.rx_buffer_size`)
**and** a stack array `uint8_t dtmp[SLAVE_UART_BUF_SIZE]` inside `uart_event_task`, whose task
stack is only `KB(8)`. Naively growing that one macro to 8 KB (as a size bump would suggest)
puts an 8 KB array on an 8 KB stack → **stack overflow on the first byte of UART RX**. The fix
**decouples** the two: `SLAVE_UART_RX_RINGBUF_SIZE` (16 KB) sizes the heap ring; the stack read
chunk stays at `SLAVE_UART_BUF_SIZE` (2 KB).

**Deliberately left untouched** (guardrails): the magic-baud download trigger, the esptool
DTR/RTS→BOOT/RST auto-reset flow and the 115200-on-reset line, the `tusb_device_task` CPU0 pin,
and the `UART_FIFO_OVF`/`UART_BUFFER_FULL` flush-on-overflow behavior.

---

## Build

```bash
./build.sh
```

`build.sh` sources ESP-IDF, layers the ESP-Prog-2 + ESP32-S3 sdkconfig defaults (matching the
Launchpad CI), builds, and also produces a **merged** single-file image for foolproof flashing.

- Override the toolchain location with `IDF_PATH=/path/to/esp-idf ./build.sh`
  (validated on **ESP-IDF v5.5.5**; the upstream CI matrix covers v5.0–v6.0).
- Target chip is **ESP32-S3** (ESP-Prog-2), selected via the layered sdkconfig defaults.

Artifacts:
- `build/bridge.bin` — the **app only** (flash at **`0x10000`**)
- `build/esp-prog2-throughput.bin` — the **merged** image (flash at **`0x0`**)

---

## Flash

⚠️ **Offset matters — this is exactly what bricked a probe during development:**

| Image | Flash offset |
|---|---|
| **Merged** (`esp-prog2-throughput.bin`, or the factory `esp-prog2.bin`) | **`0x0`** |
| **App only** (`bridge.bin`) | **`0x10000`** |

A merged image written at `0x10000` puts the bootloader into the app partition → *"Segment 0
invalid: overlaps bootloader stack" / "No bootable app partitions"* → boot loop. An app-only
image written at `0x0` bricks it the same way, inverted.

Foolproof path (merged image at `0x0`, full erase):

```bash
./flash.sh /dev/ttyACM1                 # uses esptool on PATH
./flash.sh /dev/ttyACM1 /path/to/esptool   # or an explicit esptool binary
```

Put the ESP-Prog-2 in download mode first (hold **BOOT**, tap **RESET**), then release BOOT and
power-cycle after flashing. Recovery to stock is the same command pointed at the factory
`esp-prog2.bin` (from ESP Launchpad).

To keep the factory bootloader/partition table and swap only the app:

```bash
esptool --port /dev/ttyACM1 --chip esp32s3 write-flash 0x10000 build/bridge.bin
```

Confirm *this* firmware is running (not factory): the boot log shows **`uart: queue free spaces: 40`**
(factory shows `20`).

---

## Validation

Method: a **16 MiB continuous raw flash read** of a target through the bridge (`esptool read-flash
0 ALL`) at each baud, checksummed **byte-for-byte**. The 115200 run is the trusted baseline (the
rate at which the link/hardware is known-good, and even stock firmware never dropped a byte).

| Baud | Throughput | Result |
|---|---|---|
| 115200 (baseline) | 11.5 KB/s | ✅ reference |
| 230400 | 23 KB/s | ✅ identical to baseline |
| 460800 | 46 KB/s | ✅ identical |
| 921600 | 92 KB/s | ✅ identical — **original goal** |
| 1,500,000 | 150 KB/s | ✅ identical |
| 2,000,000 | 200 KB/s | ✅ identical |
| 3,000,000 | 300 KB/s | ✅ identical |
| 4,000,000 | 400 KB/s | ✅ identical — **highest tested** |

All eight dumps share one SHA-256 (`3b859a64…c6952`), 16,777,216 bytes each. Eight different
bauds producing byte-identical output is strong proof of losslessness — a single dropped byte
shifts everything after it and shatters the hash, and different bauds have different stall/timing
patterns.

**Ceiling note:** 4.5 Mbaud could not be tested — it fails **host-side** (`termios: Invalid
argument` when setting the local port baud), because `B4000000` is the highest standard Linux
baud constant. That is a host/OS limit, **not** this firmware's throughput ceiling. The firmware's
true ceiling is somewhere above 400 KB/s and below the ~1 MB/s USB Full-Speed wall; 4 Mbaud is the
practical stopping point given standard tooling and the ESP32-S3 UART.

---

## Known limitations / trade-offs

- **Only one workload was validated** — one-directional bulk reads (target→PC). **Not** tested:
  flashing a target *through* the bridge (the PC→target path + auto-reset), interactive/latency-
  sensitive serial, and JTAG/SWD (alone or concurrent with serial). Reset and JTAG code were left
  untouched, so risk is low but unverified. Regression-test those before field use as a flasher/debugger.
- **Still silently lossy above the drain ceiling.** Overflow *probability* was slashed, but the
  drop-on-full behavior was not changed (Stage 3 was skipped). Above the drain ceiling it drops
  bytes silently rather than erroring — **always checksum high-rate captures.**
- **Higher CPU during transfers** (the 10 ms nap was removed). Mitigated by queue-blocking when
  idle, `taskYIELD()`, and the disabled CPU0 idle-watchdog. Could contend under a pathological
  concurrent JTAG + max-rate serial load (untested).
- **~+42 KB internal DRAM** permanently committed (32 KB USB ring + 12 KB UART ring + FIFOs).
  Fine today (~232 KB free at build), but less headroom for future features.
- **Fork maintainability:** future upstream updates need a re-merge; sizes are hardcoded (not
  Kconfig) by choice.

---

## What was not done, and why

The original plan had three more stages; none were necessary:

- **Stage 2** (rework `usb_sender_task` to keep the TinyUSB FIFO continuously fed, dropping the
  per-block semaphore wait) — would push closer to the ~1 MB/s USB-FS wall, but Stage 1 already
  hit 4 Mbaud losslessly, far past the 90 KB/s requirement.
- **Stage 3** (graceful backpressure instead of silent drop on ring-full) — only matters above the
  drain ceiling, which the use case never approaches.

If you ever need to saturate USB Full-Speed (~1 MB/s), Stage 2 is the lever.

---

## Provenance

- Forked from upstream `esp-usb-bridge` at commit `f5e492a` (≈ v1.2.1).
- Changes on branch `throughput-921600`, base commit `3ee3283`.
- Built and validated with **ESP-IDF v5.5.5**, target **ESP32-S3**.
- `build.sh` reproduces the validated firmware: the compiled code/data segments are
  byte-identical to the flash-tested `bridge.bin`; only the embedded build metadata
  (compile timestamp, git-describe string, ELF/image SHA) differs, as ESP-IDF stamps
  that fresh on every build.
