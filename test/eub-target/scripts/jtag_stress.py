# JTAG stress test for eub-target, driven over an ESP-Prog-2 (esp-usb-bridge) via OpenOCD + GDB.
#
# Terminal 1 -- OpenOCD (adapter = the ESP-Prog-2's esp-usb-bridge JTAG, target = esp32s3):
#     openocd -f board/esp32s3-bridge.cfg
#     # (or your openocd-esp32 esp-usb-bridge interface cfg + target/esp32s3.cfg;
#     #  the exact cfg name varies by openocd-esp32 version.)
#
# Terminal 2 -- GDB with this script:
#     xtensa-esp32s3-elf-gdb build/eub-target.elf \
#         -ex "set pagination off" \
#         -ex "target remote :3333" \
#         -x scripts/jtag_stress.py
#
# Two phases + a pass/fail summary:
#   1. Memory hammer -- halted; read+verify a known 16 KB pattern and write+verify 16 KB scratch,
#                       many iterations. Stresses JTAG memory-access throughput and integrity.
#   2. Control churn -- breakpoint on app_tick(); continue/hit/read the counter, many times.
#                       Stresses the halt/resume/breakpoint control path.
#
# Any mismatch = the ESP-Prog-2 JTAG bridge corrupted a transfer under load. Zero errors = pass.

import gdb
import time

MEM_ITERS = 300
CTRL_ITERS = 200


def addr_of(sym):
    return int(gdb.parse_and_eval("&" + sym))


def read_bytes(addr, n):
    return bytes(gdb.selected_inferior().read_memory(addr, n))


def write_bytes(addr, data):
    gdb.selected_inferior().write_memory(addr, data)


def phase_memory():
    n = int(gdb.parse_and_eval("sizeof(g_jtag_pattern)"))
    pat_addr = addr_of("g_jtag_pattern")
    scr_addr = addr_of("g_jtag_scratch")
    expect = bytes(i & 0xFF for i in range(n))

    gdb.execute("monitor halt", to_string=True)
    errs = 0
    moved = 0
    t0 = time.time()
    for it in range(MEM_ITERS):
        if read_bytes(pat_addr, n) != expect:
            errs += 1
            print("  [mem] iter %d: pattern read mismatch" % it)
        payload = bytes((it + i) & 0xFF for i in range(n))
        write_bytes(scr_addr, payload)
        if read_bytes(scr_addr, n) != payload:
            errs += 1
            print("  [mem] iter %d: scratch write/read mismatch" % it)
        moved += 3 * n
    dt = time.time() - t0
    print("Phase 1 (memory): %d iters, %.1f MB moved, %d error(s), %.1fs, %.0f KB/s"
          % (MEM_ITERS, moved / 1e6, errs, dt, (moved / 1024.0 / dt) if dt else 0))
    return errs


def phase_control():
    gdb.execute("delete", to_string=True)
    bp = gdb.Breakpoint("app_tick")
    errs = 0
    last = None
    t0 = time.time()
    for it in range(CTRL_ITERS):
        gdb.execute("continue", to_string=True)   # runs until the app_tick breakpoint hits
        cur = int(gdb.parse_and_eval("g_tick_counter"))
        if last is not None and cur <= last:
            errs += 1
            print("  [ctrl] iter %d: counter did not advance (%d -> %d)" % (it, last, cur))
        last = cur
    dt = time.time() - t0
    bp.delete()
    print("Phase 2 (control): %d halt/resume cycles, %d error(s), %.1fs, %.0f cycles/s"
          % (CTRL_ITERS, errs, dt, (CTRL_ITERS / dt) if dt else 0))
    return errs


def main():
    print("=== eub-target JTAG stress ===")
    total = 0
    total += phase_memory()
    total += phase_control()
    gdb.execute("monitor resume", to_string=True)
    print("=== RESULT: %s (%d error(s)) ===" % ("PASS" if total == 0 else "FAIL", total))


main()
