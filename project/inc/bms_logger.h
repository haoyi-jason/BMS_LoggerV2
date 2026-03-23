#ifndef BMS_LOGGER_H
#define BMS_LOGGER_H

#include "at32f413.h"
#include "ff.h"
#include <stdint.h>
#include <stdbool.h>

/* ---------- UART RX ring-buffer size ---------- */
#define UART_RX_BUF_SIZE   256

/* ---------- SD log write buffer size ---------- */
#define LOG_LINE_MAX_LEN   128

/* ---------- LED GPIO (PB0) ---------- */
#define LED_GPIO_PORT      GPIOB
#define LED_GPIO_PIN       GPIO_PINS_0

/* ---------- Callback type for user packet parser ---------- */
typedef void (*uart_rx_handler_t)(const uint8_t *data, uint16_t len);

/* ---------- UART API ---------- */
void     bms_uart_init(void);
void     bms_uart_set_baudrate(uint32_t baudrate);
void     bms_uart_register_handler(uart_rx_handler_t handler);
void     bms_uart_irq_handler(void);   /* call from USART1_IRQHandler */

/* ---------- SD Card Logger API ---------- */
void     bms_logger_init(void);
bool     bms_logger_write(const char *csv_line);   /* returns false if USB MSD is active */
void     bms_logger_flush(void);
void     bms_logger_close(void);

/* ---------- USB MSD mutex ---------- */
void     bms_usb_msd_set_active(bool active);
bool     bms_usb_msd_is_active(void);

/* ---------- LED indicator ---------- */
void     bms_led_set(bool on);
void     bms_led_toggle(void);

#endif /* BMS_LOGGER_H */
