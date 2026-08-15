# eub-target — ESP-Prog-2 test target firmware

A single universal ESP32-S3 firmware for regression- and stress-testing an ESP-Prog-2 running
the [high-throughput bridge fork](../../docs/THROUGHPUT.md). Flash it to one or two ESP32-S3 dev
boards; each board can act as a **JTAG target**, a **serial target**, or both at once.

It uses **two independent channels** so control and test data never collide:

| Channel | Where | Carries |
|---|---|---|
| **Console** | the dev board's own USB (native USB-Serial/JTAG) | logs + a command loop (`baud`, `pattern`, `status`, `help`) |
| **Bridge** | UART0 (GPIO43/44) → the ESP-Prog-2 | boot banner, ~1 Hz heartbeat, byte echo, known-pattern bursts |

## Build & flash

```bash
idf.py set-target esp32s3      # first time only
idf.py build
idf.py -p <DEVBOARD_USB_PORT> flash     # over the board's OWN usb, not the ESP-Prog-2
```
Optional: `idf.py menuconfig` → *eub-target configuration* to change the bridge UART pins/port,
default baud, or enable an LED (`EUB_TARGET_LED_GPIO`, default -1/off).

## Wiring (target board → ESP-Prog-2)

**Serial** (bridge test):
| Target | ESP-Prog-2 |
|---|---|
| GPIO43 (U0TXD) | RXD |
| GPIO44 (U0RXD) | TXD |
| GND | GND |

**JTAG** (debug/stress) — the firmware never touches these pins:
| Target | ESP-Prog-2 |
|---|---|
| GPIO39 (MTCK) | TCK |
| GPIO40 (MTDO) | TDO |
| GPIO41 (MTDI) | TDI |
| GPIO42 (MTMS) | TMS |
| GND | GND |

Wire **both** sets to the same board to run the concurrent JTAG+serial stress (§D).

## Console commands (on the dev board's own USB)

Open it with `idf.py -p <DEVBOARD_USB_PORT> monitor` (or any terminal). Commands:
- `baud <rate>` — set the bridge UART baud (300 … 5,000,000). Logs the *actual* rate achieved.
- `pattern [nbytes]` — stream a known pattern burst on the bridge (default 64 KiB).
- `status` — tick counter, baud, echo/burst counts, JTAG buffer addresses.
- `help`

---

## Regression / stress procedure

Prereqs on the bench: ESP-IDF (this repo's toolchain), `openocd-esp32`, and a serial terminal
(`picocom`). The ESP-Prog-2 is assumed on `/dev/ttyACM1` (adjust as needed).

### A. Serial bridge — both directions + baud ladder
1. Console: `idf.py -p <devboard> monitor`. Bridge: `picocom -b 115200 /dev/ttyACM1`.
2. **target→PC:** the bridge shows the banner + `[tick N] …` heartbeat. ✔ live.
3. **PC↔target:** type in `picocom` — every character comes back (echo). ✔ both directions.
4. **integrity:** on the console run `pattern 1000000`; capture the bridge output and verify the
   payload between `<PATTERN …>` and `</PATTERN>` is `byte[i] = i & 0xFF`:
   ```bash
   # capture ~1 MB from the bridge into cap.bin, then:
   python3 -c "d=open('cap.bin','rb').read(); i=d.find(b'>')+3; p=d[i:d.find(b'</PATTERN>')-2]; \
               print('OK' if all(b==(k&0xFF) for k,b in enumerate(p)) else 'MISMATCH', len(p),'bytes')"
   ```
5. **baud ladder (above normal use):** console `baud 921600` → switch `picocom` to 921600 →
   `pattern 1000000` → verify. Repeat for **1.5M, 2M, 3M, 4M**. (4M is the standard-tooling
   ceiling — `B4000000` is the top standard Linux baud; the target UART itself does 5M cleanly,
   so the wall you find is the bridge/host/wiring, not this firmware.)

### B. JTAG — connect, smoke, stress
1. OpenOCD (adapter = the ESP-Prog-2's esp-usb-bridge JTAG):
   ```bash
   openocd -f board/esp32s3-bridge.cfg    # exact cfg name varies by openocd-esp32 version
   ```
2. Smoke test (GDB):
   ```bash
   xtensa-esp32s3-elf-gdb build/eub-target.elf -ex "target remote :3333"
   (gdb) monitor halt
   (gdb) print g_tick_counter        # a value; halt froze it
   (gdb) break app_tick
   (gdb) continue                    # breakpoint hits every tick ✔ control works
   (gdb) print g_tick_counter        # advanced by 1
   ```
3. Stress (hammers the bridge's JTAG memory + control paths, verifies integrity):
   ```bash
   xtensa-esp32s3-elf-gdb build/eub-target.elf \
       -ex "set pagination off" -ex "target remote :3333" -x scripts/jtag_stress.py
   ```
   Expect `=== RESULT: PASS (0 error(s)) ===`. Any mismatch = the JTAG bridge corrupted a
   transfer under load.

### C. Flashing through the bridge (esptool + UF2)
These exercise the bridge's auto-reset + flashing paths (which our fork's RX-FIFO change touches):
- **esptool:** `esptool --port /dev/ttyACM1 --chip esp32s3 flash-id` (needs the target's EN/IO0
  wired to the ESP-Prog-2 RST/BOOT for auto-reset; else enter download mode manually). A
  read-flash + re-read + compare confirms byte-exact flashing through the bridge.
- **UF2:** convert an app to UF2, drag it onto the ESP-Prog-2's mass-storage drive, confirm the
  target runs it.

### D. Concurrent JTAG + serial (the contention test)
With one board wired for **both** JTAG and serial: start a long serial `pattern` loop *and* run
`scripts/jtag_stress.py` at the same time. Both passing confirms the fork's more-aggressive serial
task doesn't starve or corrupt JTAG under load — the one concurrency risk flagged in
[../../docs/THROUGHPUT.md](../../docs/THROUGHPUT.md).

---

*Built & compile-verified with ESP-IDF v5.5.5, target ESP32-S3. On-hardware behavior (console
command loop, pattern integrity, JTAG stress) is to be confirmed on the bench.*
