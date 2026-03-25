/* add user code begin Header */
/**
  **************************************************************************
  * @file     bms_logger.c
  * @brief    BMS Logger �� UART RX, SD card CSV logger, USB MSD mutex, LED
  **************************************************************************
  */
/* add user code end Header */

#include "bms_logger.h"
#include "at32f413_wk_config.h"
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Internal state
 * ========================================================================= */

/* --- UART ring buffer --- */
static uint8_t            rx_buf[UART_RX_BUF_SIZE];
static volatile uint16_t  rx_head = 0;   /* written by ISR */
static volatile uint16_t  rx_tail = 0;   /* read  by ISR line framer */
static uart_rx_handler_t  rx_handler = NULL;

/* Line accumulator (used inside IRQ handler) */
static uint8_t  line_buf[UART_RX_BUF_SIZE];
static uint16_t line_len = 0;

/* --- SD / FatFS logger --- */
static FATFS  fs;
static FIL    log_file;
static bool   file_open   = false;
static bool   fs_mounted  = false;

/* --- USB MSD mutex flag --- */
static volatile bool usb_msd_active = false;

/* =========================================================================
 * Helper: convert a Unix timestamp (seconds since 1970-01-01) to
 * calendar fields. The AT32F413 RTC provides a raw 32-bit second counter;
 * the application must have initialised that counter to Unix time at boot.
 * ========================================================================= */
typedef struct {
    uint16_t year;   /* e.g. 2026 */
    uint8_t  mon;    /* 1..12    */
    uint8_t  mday;   /* 1..31    */
    uint8_t  hour;   /* 0..23    */
    uint8_t  min;    /* 0..59    */
    uint8_t  sec;    /* 0..59    */
} rtc_broken_t;

static void unix_to_broken(uint32_t ts, rtc_broken_t *t)
{
    /* Days since epoch */
    uint32_t days = ts / 86400u;
    uint32_t rem  = ts % 86400u;

    t->hour = (uint8_t)(rem / 3600u);
    rem     = rem % 3600u;
    t->min  = (uint8_t)(rem / 60u);
    t->sec  = (uint8_t)(rem % 60u);

    /* Gregorian calendar */
    uint32_t y = 1970u;
    while (1) {
        uint32_t ydays = (((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0)) ? 366u : 365u;
        if (days < ydays) break;
        days -= ydays;
        y++;
    }
    t->year = (uint16_t)y;

    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t leap = (((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0)) ? 1u : 0u;
    uint8_t m;
    for (m = 0; m < 12; m++) {
        uint8_t dm = mdays[m] + ((m == 1) ? leap : 0u);
        if (days < dm) break;
        days -= dm;
    }
    t->mon  = m + 1u;
    t->mday = (uint8_t)(days + 1u);
}

/* =========================================================================
 * UART API
 * ========================================================================= */

void bms_uart_init(void)
{
    /* USART1 GPIO and peripheral already configured by wk_usart1_init().
     * We only need to enable the RXNE interrupt here.                     */
    usart_interrupt_enable(USART1, USART_RDBF_INT, TRUE);
    nvic_irq_enable(USART1_IRQn, 6, 0);
}

void bms_uart_set_baudrate(uint32_t baudrate)
{
    usart_enable(USART1, FALSE);
    usart_init(USART1, baudrate, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_parity_selection_config(USART1, USART_PARITY_NONE);
    usart_hardware_flow_control_set(USART1, USART_HARDWARE_FLOW_NONE);
    usart_transmitter_enable(USART1, TRUE);
    usart_receiver_enable(USART1, TRUE);
    usart_enable(USART1, TRUE);
    usart_interrupt_enable(USART1, USART_RDBF_INT, TRUE);
}

void bms_uart_register_handler(uart_rx_handler_t handler)
{
    rx_handler = handler;
}

void bms_uart_irq_handler(void)
{
    if (usart_flag_get(USART1, USART_RDBF_FLAG) == SET) {
        uint8_t byte = (uint8_t)(USART1->dt & 0xFFu);

        /* Store into ring buffer */
        uint16_t next = (rx_head + 1u) % UART_RX_BUF_SIZE;
        if (next != rx_tail) {   /* not full */
            rx_buf[rx_head] = byte;
            rx_head = next;
        }

        /* Line framing */
        if (byte == '\n') {
            if (rx_handler != NULL && line_len > 0u) {
                rx_handler(line_buf, line_len);
            }
            line_len = 0;
        } else {
            if (line_len < (UART_RX_BUF_SIZE - 1u)) {
                line_buf[line_len++] = byte;
            }
        }
    }
}

/* =========================================================================
 * SD Card CSV Logger
 * ========================================================================= */

void bms_logger_init(void)
{
    FRESULT fr;

    /* Mount the filesystem */
    fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        return;
    }
    fs_mounted = true;

    /* Build filename from RTC counter */
    char fname[32];
    rtc_broken_t t;
    unix_to_broken(rtc_counter_get(), &t);
    snprintf(fname, sizeof(fname), "0:%04u%02u%02u_%02u%02u%02u.csv",
             t.year, t.mon, t.mday, t.hour, t.min, t.sec);

#if FF_USE_LFN == 0
#error "LFN is DISABLE"
#endif

//    snprintf(fname, sizeof(fname), "0:20260324_.csv");
    /* Open / create log file */
    fr = f_open(&log_file, fname, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        return;
    }
    file_open = true;

    /* Write CSV header */
    UINT bw;
    FRESULT hw = f_write(&log_file, "timestamp,data\n", 15u, &bw);
    if (hw != FR_OK || bw != 15u) {
        f_close(&log_file);
        file_open = false;
    }
}

bool bms_logger_write(const char *csv_line)
{
    if (usb_msd_active) {
        return false;
    }
    if (!file_open) {
        return false;
    }

    char row[LOG_LINE_MAX_LEN + 32]; /* timestamp prefix + data + newline */
    rtc_broken_t t;
    unix_to_broken(rtc_counter_get(), &t);

    int prefix_len = snprintf(row, sizeof(row),
                              "%04u-%02u-%02u %02u:%02u:%02u,",
                              t.year, t.mon, t.mday,
                              t.hour, t.min, t.sec);
    if (prefix_len < 0) {
        return false;
    }

    /* Append user data (truncate if necessary) */
    size_t data_len = strlen(csv_line);
    size_t avail    = sizeof(row) - (size_t)prefix_len - 2u; /* -2 for '\n\0' */
    if (data_len > avail) {
        data_len = avail;
    }
    memcpy(row + prefix_len, csv_line, data_len);
    size_t total = (size_t)prefix_len + data_len;
    row[total++] = '\n';
    row[total]   = '\0';

    UINT bw;
    FRESULT fr = f_write(&log_file, row, (UINT)total, &bw);
    return (fr == FR_OK) && (bw == (UINT)total);
}

void bms_logger_flush(void)
{
    if (file_open) {
        f_sync(&log_file);
    }
}

void bms_logger_close(void)
{
    if (file_open) {
        f_close(&log_file);
        file_open = false;
    }
    if (fs_mounted) {
        f_unmount("0:");
        fs_mounted = false;
    }
}

/* =========================================================================
 * USB MSD mutex
 * ========================================================================= */

void bms_usb_msd_set_active(bool active)
{
    usb_msd_active = active;
    if (active) {
        /* Safely close the file before USB takes the bus */
        bms_logger_flush();
        bms_logger_close();
    } else {
        /* Re-mount and re-open a new log file after USB disconnects */
        bms_logger_init();
    }
}

bool bms_usb_msd_is_active(void)
{
    return usb_msd_active;
}

/* =========================================================================
 * LED indicator (PB0)
 * ========================================================================= */

void bms_led_set(bool on)
{
    if (on) {
        gpio_bits_set(LED_GPIO_PORT, LED_GPIO_PIN);
    } else {
        gpio_bits_reset(LED_GPIO_PORT, LED_GPIO_PIN);
    }
}

void bms_led_toggle(void)
{
    gpio_bits_toggle(LED_GPIO_PORT, LED_GPIO_PIN);
}
