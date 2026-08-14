# Source change list

The complete set of source modifications this fork makes to upstream `esp-usb-bridge`
(`git diff f5e492a..3ee3283`). **3 files, 8 functional edits** — everything else in the diff is
rationale comments. See [THROUGHPUT.md](THROUGHPUT.md) for the reasoning, build/flash, and validation.

## `main/public_include/tusb_config.h`
1. `CFG_TUD_CDC_RX_BUFSIZE`: **64 → 512**
2. `CFG_TUD_CDC_TX_BUFSIZE`: **64 → 512**

   CDC app-side FIFOs. `CFG_TUD_CDC_EP_BUFSIZE` is left undefined = TinyUSB's Full-Speed default
   (64 = the wire max-packet); only the FIFO depth grows, so the sender can hand TinyUSB a full
   block per blocking round-trip instead of one 64-byte packet at a time.

## `main/serial_bridge.c`
3. `USB_SEND_RINGBUFFER_SIZE`: **`2 * 1024` → `32 * 1024`**

   The `usb_sendbuf` ring — ~22 ms → ~350 ms of stall margin at 90 KB/s.

## `components/serial_handler/serial_handler.c`
4. **Added** `#define SLAVE_UART_RX_RINGBUF_SIZE KB(16)` — a new constant for the driver RX ring,
   **decoupled** from `SLAVE_UART_BUF_SIZE` (which stays `KB(2)` and remains the stack read-chunk
   `dtmp[]`). *This decoupling is the fix that avoids a stack overflow: `uart_event_task`'s stack
   is only `KB(8)`, so the ring size cannot live on the same macro as the stack buffer.*
5. `.rx_buffer_size`: **`SLAVE_UART_BUF_SIZE * 2` (4 KB) → `SLAVE_UART_RX_RINGBUF_SIZE` (16 KB)**
6. `.queue_size`: **20 → 40**
7. `uart_event_task`, `UART_DATA` case: **single read → drain-whole-ring `while` loop** (chunk-sized
   reads; `uart_read_bytes` timeout changed `portMAX_DELAY → 0` since the bytes are known-available).
   Lets a stall backlog clear in one pass instead of one chunk per queued event.
8. `uart_event_task` loop bottom: **removed** the `vTaskDelay(pdMS_TO_TICKS(10))` nap (the loop
   already blocks on `xQueueReceive(portMAX_DELAY)`; the nap only capped drain throughput).

## Deliberately NOT changed (guardrails)
- The magic-baud download trigger.
- The esptool DTR/RTS → BOOT/RST auto-reset flow and the 115200-on-reset line.
- The `tusb_device_task` CPU0 pin.
- The `UART_FIFO_OVF` / `UART_BUFFER_FULL` flush-on-overflow behavior.

## Commits
- `3ee3283` — the 8 source edits above.
- `38f9b69` — documentation + `build.sh` / `flash.sh` / README (no firmware-source change).
