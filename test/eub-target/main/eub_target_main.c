/*
 * eub-target -- a universal test target for exercising an ESP-Prog-2 (esp-usb-bridge).
 *
 * Two independent channels keep control and data from interfering:
 *   - Console : native USB-Serial/JTAG (the dev board's own USB port). Carries logs and a
 *               command loop:  baud <n> | pattern [n] | status | help
 *   - Bridge  : UART0 (GPIO43/44 by default), wired to the ESP-Prog-2. Carries a boot banner,
 *               a ~1 Hz heartbeat, a byte echo (round-trip test), and known-pattern bursts.
 *
 * JTAG target: g_tick_counter / app_tick() / g_jtag_pattern[] / g_jtag_scratch[] give a
 * debugger observable, verifiable state to halt on, read, breakpoint, and hammer. All are
 * non-static on purpose so a debugger can resolve them by symbol.
 *
 * See README.md for wiring and the regression/stress procedure.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "eub-target";

/* ---- configuration (see main/Kconfig.projbuild) ---- */
#define BRIDGE_UART       ((uart_port_t)CONFIG_EUB_TARGET_UART_NUM)
#define BRIDGE_TX_GPIO    (CONFIG_EUB_TARGET_UART_TX)
#define BRIDGE_RX_GPIO    (CONFIG_EUB_TARGET_UART_RX)
#define DEFAULT_BAUD      (CONFIG_EUB_TARGET_DEFAULT_BAUD)
#define MAX_BAUD          (CONFIG_EUB_TARGET_MAX_BAUD)
#define LED_GPIO          (CONFIG_EUB_TARGET_LED_GPIO)
#define BRIDGE_RX_BUFSZ   (8 * 1024)
#define BURST_TRIGGER     '!'          /* this byte on the bridge UART fires a default burst */
#define DEFAULT_BURST_LEN (64 * 1024)
#define BAUD_UP_GPIO      (CONFIG_EUB_TARGET_BAUD_UP_GPIO)     /* pull-up, GND = step up   */
#define BAUD_DOWN_GPIO    (CONFIG_EUB_TARGET_BAUD_DOWN_GPIO)   /* pull-up, GND = step down */
#define RGB_GPIO          (CONFIG_EUB_TARGET_RGB_GPIO)         /* WS2812 status LED (-1 off) */

/* ---- JTAG-observable state (read these from GDB) ---- */
volatile uint32_t g_tick_counter   = 0;   /* ++ every app_tick(); a halt should freeze it */
volatile uint32_t g_led_state      = 0;
volatile uint32_t g_echo_bytes     = 0;   /* total bytes echoed on the bridge UART */
volatile uint32_t g_pattern_bursts = 0;   /* number of pattern bursts sent */
volatile uint32_t g_current_baud   = DEFAULT_BAUD;

/* ---- JTAG stress buffers (documented pattern: byte[i] == (i & 0xFF)) ---- */
#define JTAG_BUF_LEN (16 * 1024)
uint8_t g_jtag_pattern[JTAG_BUF_LEN];     /* read-back + verify target */
uint8_t g_jtag_scratch[JTAG_BUF_LEN];     /* write + read-back + verify target */

static bool s_burst_active = false;

/* ---- baud ladder (stepped by the GPIO up/down buttons and re-synced by the console command) ---- */
static const uint32_t BAUD_LADDER[] = {
    9600, 115200, 230400, 460800, 921600, 1500000, 2000000, 4000000, 4100000,
};
#define BAUD_LADDER_N ((int)(sizeof(BAUD_LADDER) / sizeof(BAUD_LADDER[0])))
static int s_baud_index = 1;   /* current rung; initialised from DEFAULT_BAUD in app_main */

/* Nearest ladder rung to an arbitrary baud (keeps the button index sane after a console `baud`). */
static int ladder_index_for(uint32_t baud)
{
    int best = 0;
    uint32_t bestd = 0xFFFFFFFFu;
    for (int i = 0; i < BAUD_LADDER_N; i++) {
        uint32_t d = (BAUD_LADDER[i] > baud) ? (BAUD_LADDER[i] - baud) : (baud - BAUD_LADDER[i]);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

/* ---- WS2812 status LED: base color = current baud rung, green flash on echo, magenta on burst ---- */
static led_strip_handle_t s_strip = NULL;
static volatile int64_t s_last_rx_us = 0;   /* last bridge-UART RX time (for the green echo flash) */
static int64_t s_boot_until_us = 0;         /* show white until this time */

/* Dim base color per ladder rung (kept away from pure green/magenta so the overlays stand out). */
static const uint8_t RUNG_RGB[BAUD_LADDER_N][3] = {
    { 40,  0,  0 },  /* 9600     red    */
    { 40, 14,  0 },  /* 115200   orange */
    { 34, 30,  0 },  /* 230400   yellow */
    { 14, 34,  0 },  /* 460800   lime   */
    {  0, 34, 14 },  /* 921600   spring */
    {  0, 26, 34 },  /* 1.5M     cyan   */
    {  0, 10, 40 },  /* 2M       blue   */
    { 22,  0, 40 },  /* 4M       violet */
    { 40,  0, 24 },  /* 4.1M     pink   */
};
_Static_assert(sizeof(RUNG_RGB) / sizeof(RUNG_RGB[0]) == BAUD_LADDER_N,
               "RUNG_RGB must have one color per baud-ladder rung");

/* Recompute the LED color from current state. Called ~10 Hz from tick_task. */
static void led_update(void)
{
    if (!s_strip) {
        return;
    }
    uint8_t r, g, b;
    int64_t now = esp_timer_get_time();
    if (now < s_boot_until_us) {                 /* boot: white */
        r = 60; g = 60; b = 60;
    } else if (s_burst_active) {                  /* pattern burst: magenta */
        r = 60; g = 0; b = 60;
    } else if (now - s_last_rx_us < 200000) {     /* recent echo (200 ms): green */
        r = 0; g = 60; b = 0;
    } else {                                      /* base: current baud rung */
        r = RUNG_RGB[s_baud_index][0];
        g = RUNG_RGB[s_baud_index][1];
        b = RUNG_RGB[s_baud_index][2];
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void jtag_buffers_init(void)
{
    for (int i = 0; i < JTAG_BUF_LEN; i++) {
        g_jtag_pattern[i] = (uint8_t)(i & 0xFF);
    }
    memset(g_jtag_scratch, 0, sizeof(g_jtag_scratch));
}

/* Breakpoint target: set a breakpoint here from GDB and it should hit every tick. */
void app_tick(void)
{
    g_tick_counter++;
#if LED_GPIO >= 0
    g_led_state ^= 1u;
    gpio_set_level((gpio_num_t)LED_GPIO, g_led_state);
#endif
}

/* ---- bridge UART output helpers (uart_write_bytes is thread-safe) ---- */
static void bridge_write(const void *data, size_t len)
{
    uart_write_bytes(BRIDGE_UART, data, len);
}

static void bridge_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        bridge_write(buf, (size_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf)));
    }
}

/* Stream a deterministic, checksummable pattern on the bridge UART, framed by markers so the
 * capture is easy to locate. Payload is exactly `nbytes` of byte[k] = k & 0xFF. */
static void pattern_burst(uint32_t nbytes)
{
    if (s_burst_active) {
        ESP_LOGW(TAG, "burst already active, ignoring request");
        return;
    }
    s_burst_active = true;
    ESP_LOGI(TAG, "pattern burst: %" PRIu32 " bytes at %" PRIu32 " baud", nbytes, g_current_baud);
    bridge_printf("\r\n<PATTERN len=%" PRIu32 " fmt=byte[i]=i&0xFF>\r\n", nbytes);

    uint8_t chunk[512];
    uint32_t sent = 0;
    while (sent < nbytes) {
        uint32_t n = nbytes - sent;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = (uint8_t)((sent + i) & 0xFF);
        }
        bridge_write(chunk, n);
        sent += n;
    }
    uart_wait_tx_done(BRIDGE_UART, portMAX_DELAY);
    bridge_printf("\r\n</PATTERN>\r\n");
    g_pattern_bursts++;
    s_burst_active = false;
}

static void set_bridge_baud(uint32_t baud)
{
    if (baud < 300) {
        baud = 300;
    }
    if (baud > (uint32_t)MAX_BAUD) {
        baud = (uint32_t)MAX_BAUD;
    }
    uart_wait_tx_done(BRIDGE_UART, pdMS_TO_TICKS(200));
    esp_err_t err = uart_set_baudrate(BRIDGE_UART, baud);
    uint32_t actual = 0;
    uart_get_baudrate(BRIDGE_UART, &actual);
    g_current_baud = actual;
    s_baud_index = ladder_index_for(actual);
    /* Log to the console (clean); also announce on the bridge at the NEW rate -- the operator
     * will see garbage on the bridge until they switch their end to match. */
    ESP_LOGI(TAG, "baud requested %" PRIu32 " -> %s, actual %" PRIu32, baud, esp_err_to_name(err), actual);
    bridge_printf("\r\n<BAUD now=%" PRIu32 ">\r\n", actual);
}

/* Move `delta` rungs along the ladder (clamped at both ends) and apply the new baud. */
static void step_baud(int delta)
{
    int i = s_baud_index + delta;
    if (i < 0) {
        i = 0;
    }
    if (i >= BAUD_LADDER_N) {
        i = BAUD_LADDER_N - 1;
    }
    if (i == s_baud_index) {
        return;   /* already at an end */
    }
    ESP_LOGI(TAG, "button -> ladder[%d] = %" PRIu32, i, BAUD_LADDER[i]);
    set_bridge_baud(BAUD_LADDER[i]);   /* re-syncs s_baud_index to this rung */
}

#if (BAUD_UP_GPIO >= 0) || (BAUD_DOWN_GPIO >= 0)
/* Poll the up/down buttons: active-low (GND = pressed), one step per press, debounced. */
static void button_task(void *arg)
{
    int prev_up = 1;
    int prev_down = 1;
    for (;;) {
#if BAUD_UP_GPIO >= 0
        int lvl_up = gpio_get_level((gpio_num_t)BAUD_UP_GPIO);
        if (prev_up == 1 && lvl_up == 0) {          /* falling edge = press */
            step_baud(+1);
            vTaskDelay(pdMS_TO_TICKS(50));          /* debounce settle */
        }
        prev_up = lvl_up;
#endif
#if BAUD_DOWN_GPIO >= 0
        int lvl_down = gpio_get_level((gpio_num_t)BAUD_DOWN_GPIO);
        if (prev_down == 1 && lvl_down == 0) {
            step_baud(-1);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        prev_down = lvl_down;
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

/* ---- tasks ---- */
static void bridge_rx_task(void *arg)
{
    uint8_t *buf = malloc(1024);
    if (!buf) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        int n = uart_read_bytes(BRIDGE_UART, buf, 1024, pdMS_TO_TICKS(100));
        if (n > 0) {
            uart_write_bytes(BRIDGE_UART, buf, n);   /* exact round-trip echo */
            g_echo_bytes += (uint32_t)n;
            s_last_rx_us = esp_timer_get_time();     /* drives the green echo flash */
            if (memchr(buf, BURST_TRIGGER, (size_t)n) != NULL) {
                pattern_burst(DEFAULT_BURST_LEN);
            }
        }
    }
}

static void tick_task(void *arg)
{
    int hb = 0;
    for (;;) {
        app_tick();                       /* ~10 Hz: keeps a debugger's halt/step busy */
        led_update();                     /* refresh the WS2812 from current state */
        if (++hb >= 10) {                 /* ~1 Hz heartbeat on the bridge */
            hb = 0;
            if (!s_burst_active) {
                bridge_printf("[tick %" PRIu32 "] baud=%" PRIu32 " echo=%" PRIu32 " bursts=%" PRIu32 "\r\n",
                              g_tick_counter, g_current_baud, g_echo_bytes, g_pattern_bursts);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void handle_command(const char *line)
{
    if (strncmp(line, "baud", 4) == 0) {
        uint32_t b = (uint32_t)strtoul(line + 4, NULL, 0);
        if (b == 0) {
            printf("usage: baud <rate>  (300..%d)\n", MAX_BAUD);
        } else {
            set_bridge_baud(b);
        }
    } else if (strncmp(line, "pattern", 7) == 0) {
        uint32_t n = (uint32_t)strtoul(line + 7, NULL, 0);
        pattern_burst(n ? n : DEFAULT_BURST_LEN);
    } else if (strcmp(line, "status") == 0) {
        printf("tick=%" PRIu32 " baud=%" PRIu32 " echo_bytes=%" PRIu32 " bursts=%" PRIu32 "\n",
               g_tick_counter, g_current_baud, g_echo_bytes, g_pattern_bursts);
        printf("g_jtag_pattern=%p g_jtag_scratch=%p len=%d (byte[i]=i&0xFF)\n",
               (void *)g_jtag_pattern, (void *)g_jtag_scratch, JTAG_BUF_LEN);
    } else {
        printf("commands: baud <rate> | pattern [nbytes] | status | help\n");
    }
}

static void console_task(void *arg)
{
    char line[80];
    int pos = 0;
    uint8_t c;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[pos] = '\0';
            if (pos > 0) {
                handle_command(line);
            }
            pos = 0;
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)c;
        }
    }
}

void app_main(void)
{
    jtag_buffers_init();

    /* Console on the native USB-Serial/JTAG (the dev board's own USB). */
    usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj));
    usb_serial_jtag_vfs_use_driver();

    /* Optional LED (plain GPIO toggle; -1 disables, compiled out). */
#if LED_GPIO >= 0
    gpio_config_t led_io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_io);
#endif

    /* WS2812 status LED (base = baud rung, green = echo, magenta = burst). Non-fatal on failure. */
    s_boot_until_us = esp_timer_get_time() + 1000000;   /* white for ~1 s at boot */
    if (RGB_GPIO >= 0) {
        led_strip_config_t sc = {
            .strip_gpio_num = RGB_GPIO,
            .max_leds = 1,
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,
            .led_model = LED_MODEL_WS2812,
        };
        led_strip_rmt_config_t rc = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
        };
        esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &s_strip);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "WS2812 init on GPIO%d failed: %s (LED disabled)", RGB_GPIO, esp_err_to_name(e));
            s_strip = NULL;
        }
    }

    /* Bridge UART to the ESP-Prog-2. */
    uart_config_t uc = {
        .baud_rate = DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART, BRIDGE_RX_BUFSZ, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART, BRIDGE_TX_GPIO, BRIDGE_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    g_current_baud = DEFAULT_BAUD;
    s_baud_index = ladder_index_for(DEFAULT_BAUD);

    /* Baud step buttons (input + internal pull-up, active-low; -1 disables). */
#if (BAUD_UP_GPIO >= 0) || (BAUD_DOWN_GPIO >= 0)
    uint64_t btn_mask = 0;
#if BAUD_UP_GPIO >= 0
    btn_mask |= 1ULL << BAUD_UP_GPIO;
#endif
#if BAUD_DOWN_GPIO >= 0
    btn_mask |= 1ULL << BAUD_DOWN_GPIO;
#endif
    gpio_config_t btn_io = {
        .pin_bit_mask = btn_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_io);
#endif

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "eub-target %s up. console=USB-JTAG, bridge=UART%d GPIO%d(TX)/%d(RX) @ %d baud",
             app->version, (int)BRIDGE_UART, BRIDGE_TX_GPIO, BRIDGE_RX_GPIO, DEFAULT_BAUD);
    ESP_LOGI(TAG, "JTAG buffers: g_jtag_pattern=%p g_jtag_scratch=%p len=%d (byte[i]=i&0xFF)",
             (void *)g_jtag_pattern, (void *)g_jtag_scratch, JTAG_BUF_LEN);
    ESP_LOGI(TAG, "console commands: baud <n> | pattern [n] | status | help");
    ESP_LOGI(TAG, "baud buttons: up=GPIO%d down=GPIO%d (GND=press); ladder rungs=%d, start=%" PRIu32,
             BAUD_UP_GPIO, BAUD_DOWN_GPIO, BAUD_LADDER_N, BAUD_LADDER[s_baud_index]);
    ESP_LOGI(TAG, "WS2812 status LED: GPIO%d %s", RGB_GPIO, s_strip ? "active" : "disabled");

    bridge_printf("\r\n=== eub-target %s  mac=%02x:%02x:%02x:%02x:%02x:%02x  baud=%d ===\r\n",
                  app->version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], DEFAULT_BAUD);
    bridge_printf("echo active; send '%c' for a %d-byte pattern burst\r\n", BURST_TRIGGER, DEFAULT_BURST_LEN);

    xTaskCreate(bridge_rx_task, "bridge_rx", 4096, NULL, 10, NULL);
    xTaskCreate(tick_task,      "tick",      3072, NULL, 5,  NULL);
    xTaskCreate(console_task,   "console",   4096, NULL, 5,  NULL);
#if (BAUD_UP_GPIO >= 0) || (BAUD_DOWN_GPIO >= 0)
    xTaskCreate(button_task,    "buttons",   3072, NULL, 6,  NULL);
#endif
}
