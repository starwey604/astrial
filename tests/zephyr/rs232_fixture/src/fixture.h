/* SPDX-License-Identifier: MIT */
#ifndef ASTRIAL_RS232_FIXTURE_H
#define ASTRIAL_RS232_FIXTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIXTURE_LINE_SIZE 512
#define FIXTURE_MAX_COUNT (16U * 1024U * 1024U)
/* All masks use the PC/DTE perspective. Logical 1 means asserted. */
enum { F_CTS = 1, F_DSR = 2, F_DCD = 4, F_RI = 8, F_RTS = 16, F_DTR = 32 };
enum fixture_flow { F_NONE, F_XONXOFF, F_RTS_GATE };
enum fixture_job { F_IDLE, F_TX, F_RX, F_ECHO };
struct fixture_config {
    uint32_t baud;
    uint8_t bits, stops;
    char parity;
    enum fixture_flow flow;
};
struct fixture_io {
    void *ctx;
    int (*configure)(void *, const struct fixture_config *);
    int (*set_lines)(void *, uint8_t);
    uint8_t (*inputs)(void *);
    size_t (*write)(void *, const uint8_t *, size_t);
    bool (*tx_idle)(void *); /* Includes hardware shift-register completion. */
    void (*pause)(void *, bool);
    void (*reset)(void *); /* Abort TX, discard RX, leave UART enabled. */
    bool (*emit)(void *, const char *, size_t); /* Entire line, or no bytes. */
};
struct fixture {
    struct fixture_io io;
    struct fixture_config config;
    enum fixture_job job;
    uint32_t job_seq, goal, progress, seed, mismatches, first_bad;
    uint64_t deadline, rx_bytes, tx_queued, unexpected, errors, control_lost, rx_dropped;
    uint8_t outputs;
    bool xoff, fault, discarding, completion_pending;
    size_t line_len, pending_len;
    char line[FIXTURE_LINE_SIZE], pending[FIXTURE_LINE_SIZE];
    char last[FIXTURE_LINE_SIZE];
};
/* A failed init must not be advertised as ready. No dynamic allocation. */
bool fixture_init(struct fixture *, const struct fixture_io *);
bool fixture_ready(struct fixture *); /* Retries one backpressured output line. */
void fixture_control(struct fixture *, uint8_t byte, uint64_t now_ms);
void fixture_control_overflow(struct fixture *, uint32_t dropped);
void fixture_receive(struct fixture *, const uint8_t *, size_t);
void fixture_rx_overflow(struct fixture *, uint32_t dropped);
void fixture_uart_error(struct fixture *, uint32_t observations);
void fixture_tick(struct fixture *, uint64_t now_ms);
uint8_t fixture_pattern(const struct fixture_config *, uint32_t seed, uint32_t offset);
#endif
