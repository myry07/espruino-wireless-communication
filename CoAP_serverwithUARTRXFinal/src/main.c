#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <stdio.h>

#define UART_NODE DT_NODELABEL(uart0)
#define UART_DATA_MAX_SIZE 256
#define UART_MSG_QUEUE_SIZE 10

K_MSGQ_DEFINE(uart_msgq, UART_DATA_MAX_SIZE, UART_MSG_QUEUE_SIZE, 4);

static const struct device *uart_dev;
static char temp_buf[UART_DATA_MAX_SIZE];
static int temp_pos = 0;

static void uart_rx_cb(const struct device *dev, void *user_data)
{
    char c;
    while (uart_fifo_read(dev, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            temp_buf[temp_pos] = '\0';
            k_msgq_put(&uart_msgq, temp_buf, K_NO_WAIT);
            temp_pos = 0;
        } else if (temp_pos < sizeof(temp_buf) - 1) {
            temp_buf[temp_pos++] = c;
        } else {
            temp_pos = 0; // 溢出丢弃
        }
    }
}

void main(void)
{
    printk("UART echo example start\n");

    uart_dev = DEVICE_DT_GET(UART_NODE);
    if (!device_is_ready(uart_dev)) {
        printk("UART not ready\n");
        return;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_rx_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    char line[UART_DATA_MAX_SIZE];

    while (1) {
        if (k_msgq_get(&uart_msgq, line, K_FOREVER) == 0) {
            printk("Got: %s\n", line);
            uart_poll_out(uart_dev, '\n');
            for (char *p = line; *p; p++) {
                uart_poll_out(uart_dev, *p); // 回显
            }
            uart_poll_out(uart_dev, '\n');
        }
    }
}
