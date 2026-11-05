/* SPDX-License-Identifier: MIT */
#include "fixture.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <fsl_lpuart.h>
#include <errno.h>

#define TARGET DT_NODELABEL(flexcomm6_lpuart6)
static const struct device *const control = DEVICE_DT_GET(DT_NODELABEL(flexcomm4_lpuart4));
static const struct device *const target = DEVICE_DT_GET(TARGET);
static const struct gpio_dt_spec outputs[] = {
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pc_cts_gpios),
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pc_dsr_gpios),
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pc_dcd_gpios),
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pc_ri_gpios),
};
static const struct gpio_dt_spec inputs[] = {
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pc_rts_gpios),
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pc_dtr_gpios),
};
RING_BUF_DECLARE(control_rx, 1024);
RING_BUF_DECLARE(control_tx, 4096);
RING_BUF_DECLARE(target_rx, 8192);
RING_BUF_DECLARE(target_tx, 8192);
static struct fixture fixture;
/* Protected by irq_lock on this single-core image; never accessed unlocked by main. */
static bool target_paused;
static uint32_t control_lost, target_dropped, target_errors;

static void uart_isr(const struct device *dev, void *unused)
{
    (void)unused;
    bool is_target = dev == target;
    struct ring_buf *rx = is_target ? &target_rx : &control_rx;
    struct ring_buf *tx = is_target ? &target_tx : &control_tx;
    (void)uart_irq_update(dev);
    int error = uart_err_check(dev);
    if (error > 0) {
        if (is_target) ++target_errors; /* Flag observations, not corrupt-byte count. */
        else ++control_lost;
    }
    while (uart_irq_rx_ready(dev)) {
        uint8_t data[32];
        int n = uart_fifo_read(dev, data, sizeof(data));
        if (n <= 0) break;
        uint32_t accepted = ring_buf_put(rx, data, (uint32_t)n);
        if (is_target) target_dropped += (uint32_t)n - accepted;
        else control_lost += (uint32_t)n - accepted;
    }
    error = uart_err_check(dev);
    if (error > 0) {
        if (is_target) ++target_errors;
        else ++control_lost;
    }
    if (uart_irq_tx_ready(dev)) {
        if (is_target && target_paused) { uart_irq_tx_disable(dev); return; }
        uint8_t data[32];
        uint32_t n = ring_buf_peek(tx, data, sizeof(data));
        int sent = n ? uart_fifo_fill(dev, data, (int)n) : 0;
        if (sent > 0) (void)ring_buf_get(tx, data, (uint32_t)sent);
        if (ring_buf_is_empty(tx)) uart_irq_tx_disable(dev);
    }
}
static size_t write_target(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    unsigned int key = irq_lock();
    uint32_t n = ring_buf_put(&target_tx, data, (uint32_t)len);
    if (n && !target_paused) uart_irq_tx_enable(target);
    irq_unlock(key);
    return n;
}
static bool emit_control(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    unsigned int key = irq_lock();
    bool fits = ring_buf_space_get(&control_tx) >= len;
    if (fits) {
        (void)ring_buf_put(&control_tx, (const uint8_t *)data, (uint32_t)len);
        uart_irq_tx_enable(control);
    }
    irq_unlock(key);
    return fits;
}
static void pause_target(void *ctx, bool paused)
{
    (void)ctx;
    unsigned int key = irq_lock();
    target_paused = paused;
    if (paused) uart_irq_tx_disable(target);
    else if (!ring_buf_is_empty(&target_tx)) uart_irq_tx_enable(target);
    irq_unlock(key);
}
static bool target_idle(void *ctx)
{
    (void)ctx;
    unsigned int key = irq_lock();
    /* uart_irq_tx_complete is ISR-only in the generic API; read the MCX flag here. */
    bool idle = ring_buf_is_empty(&target_tx) &&
        (LPUART_GetStatusFlags((LPUART_Type *)DT_REG_ADDR(TARGET)) & kLPUART_TransmissionCompleteFlag);
    irq_unlock(key);
    return idle;
}
static void reset_target(void *ctx)
{
    (void)ctx;
    unsigned int key = irq_lock();
    uart_irq_tx_disable(target);
    uart_irq_rx_disable(target);
    uart_irq_err_disable(target);
    LPUART_Type *base = (LPUART_Type *)DT_REG_ADDR(TARGET);
    uint32_t ctrl = base->CTRL;
    /* Only this board backend touches registers. No clock gating / driver Deinit. */
    base->CTRL = ctrl & ~(LPUART_CTRL_TE_MASK | LPUART_CTRL_RE_MASK);
    base->FIFO |= LPUART_FIFO_TXFLUSH_MASK | LPUART_FIFO_RXFLUSH_MASK;
    (void)uart_err_check(target);
    ring_buf_reset(&target_tx);
    ring_buf_reset(&target_rx);
    base->CTRL = ctrl;
    uart_irq_rx_enable(target);
    uart_irq_err_enable(target);
    irq_unlock(key);
}
static int configure_target(void *ctx, const struct fixture_config *c)
{
    (void)ctx;
    struct uart_config config = {
        .baudrate = c->baud,
        .parity = c->parity == 'E' ? UART_CFG_PARITY_EVEN :
                  c->parity == 'O' ? UART_CFG_PARITY_ODD : UART_CFG_PARITY_NONE,
        .stop_bits = c->stops == 2 ? UART_CFG_STOP_BITS_2 : UART_CFG_STOP_BITS_1,
        .data_bits = c->bits == 7 ? UART_CFG_DATA_BITS_7 : UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    uart_irq_rx_disable(target);
    uart_irq_err_disable(target);
    int ret = uart_configure(target, &config);
    /* Host must quiesce the data path before CONFIG. Old queued RX is discarded. */
    unsigned int key = irq_lock();
    ring_buf_reset(&target_rx);
    irq_unlock(key);
    uart_irq_rx_enable(target);
    uart_irq_err_enable(target);
    return ret;
}
static int set_lines(void *ctx, uint8_t mask)
{
    (void)ctx;
    int error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(outputs); ++i) {
        int e = gpio_pin_set_dt(&outputs[i], !!(mask & (1U << i)));
        if (e) error = e;
    }
    return error;
}
static uint8_t get_inputs(void *ctx)
{
    (void)ctx;
    uint8_t mask = 0;
    if (gpio_pin_get_dt(&inputs[0]) > 0) mask |= F_RTS;
    if (gpio_pin_get_dt(&inputs[1]) > 0) mask |= F_DTR;
    return mask;
}
static uint32_t take_rx(struct ring_buf *ring, uint8_t *data, uint32_t size)
{
    unsigned int key = irq_lock();
    uint32_t n = ring_buf_get(ring, data, size);
    irq_unlock(key);
    return n;
}
int main(void)
{
    if (!device_is_ready(control) || !device_is_ready(target)) return -ENODEV;
    for (size_t i = 0; i < ARRAY_SIZE(outputs); ++i)
        if (!gpio_is_ready_dt(&outputs[i]) || gpio_pin_configure_dt(&outputs[i], GPIO_OUTPUT_INACTIVE))
            return -ENODEV;
    for (size_t i = 0; i < ARRAY_SIZE(inputs); ++i)
        if (!gpio_is_ready_dt(&inputs[i]) || gpio_pin_configure_dt(&inputs[i], GPIO_INPUT))
            return -ENODEV;
    if (uart_irq_callback_user_data_set(control, uart_isr, NULL) ||
        uart_irq_callback_user_data_set(target, uart_isr, NULL)) return -ENOTSUP;
    uart_irq_rx_enable(control);
    uart_irq_err_enable(control);
    struct fixture_io io = {
        .configure = configure_target, .set_lines = set_lines, .inputs = get_inputs,
        .write = write_target, .tx_idle = target_idle, .pause = pause_target,
        .reset = reset_target, .emit = emit_control,
    };
    if (!fixture_init(&fixture, &io))
        (void)emit_control(NULL, "V1 0 ERR INIT\n", sizeof("V1 0 ERR INIT\n") - 1);
    for (;;) {
        unsigned int key = irq_lock();
        uint32_t lost = control_lost;
        control_lost = 0;
        /* Discard the whole damaged control buffer, not a possibly executable suffix. */
        if (lost) ring_buf_reset(&control_rx);
        irq_unlock(key);
        if (lost) fixture_control_overflow(&fixture, lost);
        uint64_t now = (uint64_t)k_uptime_get();
        /* Bound control work so a command flood cannot starve data RX / deadlines. */
        for (unsigned int i = 0; i < 256 && fixture_ready(&fixture); ++i) {
            uint8_t b;
            if (!take_rx(&control_rx, &b, 1)) break;
            fixture_control(&fixture, b, now);
        }
        /* Keep the control parser responsive even if the peer floods the target UART. */
        for (unsigned int i = 0; i < 64; ++i) {
            uint8_t data[128];
            key = irq_lock();
            uint32_t n = ring_buf_get(&target_rx, data, sizeof(data));
            uint32_t errors = target_errors, dropped = target_dropped;
            target_errors = target_dropped = 0;
            irq_unlock(key);
            if (errors) fixture_uart_error(&fixture, errors);
            if (dropped) fixture_rx_overflow(&fixture, dropped);
            if (!errors && !dropped && n) fixture_receive(&fixture, data, n);
            if (!n || errors || dropped) break;
        }
        fixture_tick(&fixture, now);
        k_sleep(K_MSEC(1));
    }
}
