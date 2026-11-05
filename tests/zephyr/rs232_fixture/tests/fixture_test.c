/* SPDX-License-Identifier: MIT */
#include "fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
    uint8_t tx[65536], inputs, outputs;
    size_t tx_len, capacity;
    bool wire_busy, paused, blocked;
    unsigned configure_calls, reset_calls, line_calls, configure_failures;
    bool line_fail;
    struct fixture_config configured;
    char output[65536];
    size_t output_len;
};
static struct fixture f;
static struct fake hw;
static int configure(void *p, const struct fixture_config *c)
{
    struct fake *h = p;
    ++h->configure_calls;
    if (h->configure_failures) { --h->configure_failures; return -1; }
    h->configured = *c; return 0;
}
static int set_lines(void *p, uint8_t m)
{
    struct fake *h = p;
    ++h->line_calls;
    if (h->line_fail) return -1;
    h->outputs = m; return 0;
}
static uint8_t inputs(void *p) { return ((struct fake *)p)->inputs; }
static size_t write_data(void *p, const uint8_t *data, size_t n)
{
    struct fake *h = p;
    if (n > h->capacity) n = h->capacity;
    assert(h->tx_len + n <= sizeof(h->tx));
    memcpy(h->tx + h->tx_len, data, n);
    h->tx_len += n; h->capacity -= n;
    if (n) h->wire_busy = true;
    return n;
}
static bool idle(void *p) { return !((struct fake *)p)->wire_busy; }
static void pause_tx(void *p, bool paused) { ((struct fake *)p)->paused = paused; }
static void reset(void *p)
{
    struct fake *h = p;
    ++h->reset_calls; h->wire_busy = false; h->capacity = 64;
}
static bool emit(void *p, const char *s, size_t n)
{
    struct fake *h = p;
    if (h->blocked) return false;
    assert(h->output_len + n < sizeof(h->output));
    memcpy(h->output + h->output_len, s, n);
    h->output_len += n; h->output[h->output_len] = 0;
    return true;
}
static struct fixture_io io(void)
{
    return (struct fixture_io){&hw, configure, set_lines, inputs, write_data,
        idle, pause_tx, reset, emit};
}
static void init(void)
{
    memset(&hw, 0, sizeof(hw)); hw.capacity = 64;
    struct fixture_io ops = io();
    assert(fixture_init(&f, &ops));
    assert(f.config.baud == 115200 && !hw.outputs && !hw.paused);
}
static void clear_output(void) { hw.output_len = 0; hw.output[0] = 0; }
static void cmd_at(const char *s, uint64_t now)
{
    assert(fixture_ready(&f));
    for (; *s; ++s) { assert(fixture_ready(&f)); fixture_control(&f, (uint8_t)*s, now); }
    fixture_control(&f, '\n', now);
}
static void cmd(const char *s) { cmd_at(s, 100); }
static void contains(const char *s)
{
    if (!strstr(hw.output, s)) { fprintf(stderr, "Missing [%s] in [%s]\n", s, hw.output); assert(0); }
}
static void test_protocol(void)
{
    init(); cmd("V1 1 CAPS"); contains("break=0 mark_space=0 native_rtscts=0");
    contains("flow=NONE,XONXOFF,RTS_GATE");
    const char *bad[] = {"", "V2 1 CAPS", "V1 0 CAPS", "V1 -1 CAPS", "V1 4294967296 CAPS",
        "V1 +1 CAPS", "V1 1 CAPS x", "V1 1 TX_PATTERN 0 0 5000", "V1 1 TX_PATTERN 1 256 5000",
        "V1 1 TX_PATTERN 16777217 0 5000", "V1 1 ECHO 1 0", "V1 1 ECHO 1 3600001",
        "V1 1 ECHO 1 5 x", "V1 1 a b c d e f g h i j"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        clear_output(); cmd(bad[i]); contains(" ERR "); assert(f.job == F_IDLE);
    }
    clear_output(); cmd("V1 4294967295 GET_LINES\r"); contains("V1 4294967295 OK");
    clear_output();
    for (unsigned i = 0; i < FIXTURE_LINE_SIZE+20; ++i) fixture_control(&f, 'A', 0);
    fixture_control(&f, '\n', 0); contains("V1 0 ERR CONTROL_LINE");
    cmd("V1 3 CAPS"); contains("V1 3 OK");
    clear_output(); fixture_control(&f, 0, 0); cmd("V1 4 STOP"); contains("CONTROL_LINE");
    fixture_control_overflow(&f, 7); cmd("V1 5 SET_LINES 1 1 1 1");
    assert(!hw.outputs && f.control_lost == 7);
    cmd("V1 6 GET_LINES"); contains("V1 6 OK");
}
static void test_config_and_lines(void)
{
    init();
    for (uint8_t m = 0; m < 16; ++m) {
        char b[80];
        snprintf(b, sizeof(b), "V1 1 SET_LINES %u %u %u %u", !!(m&1), !!(m&2), !!(m&4), !!(m&8));
        cmd(b); assert(hw.outputs == m && f.outputs == m);
    }
    hw.inputs = F_RTS | F_DTR;
    clear_output(); cmd("V1 2 GET_LINES"); contains("cts=1 dsr=1 dcd=1 ri=1 rts=1 dtr=1");
    cmd("V1 3 SET_LINES 2 0 0 0"); assert(hw.outputs == 15);
    unsigned calls = hw.configure_calls;
    const char *bad[] = {"CONFIG 1000000 8 N 1 NONE", "CONFIG 9600 6 N 1 NONE",
        "CONFIG 9600 8 M 1 NONE", "CONFIG 9600 8 S 1 NONE", "CONFIG 9600 8 N 3 NONE",
        "CONFIG 9600 8 N 1 HARDWARE", "CONFIG 9600 8 NN 1 NONE", "CONFIG 9600 8 N 1"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        char b[100]; snprintf(b, sizeof(b), "V1 1 %s", bad[i]); cmd(b);
        assert(hw.configure_calls == calls);
    }
    const unsigned rates[] = {1200,2400,4800,9600,19200,38400,57600,115200,230400};
    for (size_t i = 0; i < sizeof(rates)/sizeof(rates[0]); ++i)
        for (unsigned bits = 7; bits <= 8; ++bits)
            for (unsigned stops = 1; stops <= 2; ++stops)
                for (unsigned parity = 0; parity < 3; ++parity) {
                    char b[100]; snprintf(b, sizeof(b), "V1 1 CONFIG %u %u %c %u NONE", rates[i], bits, "NOE"[parity], stops);
                    cmd(b); assert(hw.configured.baud == rates[i] && hw.configured.bits == bits &&
                        hw.configured.stops == stops && hw.configured.parity == "NOE"[parity]);
                }
    struct fixture_config previous = f.config;
    hw.configure_failures = 1;
    clear_output(); cmd("V1 2 CONFIG 9600 7 O 2 NONE"); contains("ERR IO");
    assert(!f.fault && !memcmp(&f.config, &previous, sizeof(previous)));
    assert(!memcmp(&hw.configured, &previous, sizeof(previous)));
    hw.configure_failures = 2; cmd("V1 3 CONFIG 9600 7 O 2 NONE"); assert(f.fault);
    cmd("V1 4 TX_PATTERN 1 0 1000"); assert(f.job == F_IDLE);
    cmd("V1 5 STOP"); assert(!f.fault && !hw.outputs);
    cmd("V1 6 ECHO 10 5000");
    hw.line_fail = true; cmd("V1 7 SET_LINES 0 0 0 0"); assert(f.fault && f.job == F_IDLE);
    contains("result=IO_ERROR");
    clear_output(); cmd("V1 8 GET_LINES"); contains("ERR FAULT");
    cmd("V1 7 STOP"); assert(f.fault);
    hw.line_fail = false; cmd("V1 8 STOP"); assert(!f.fault);
    init(); hw.configure_failures = 1;
    struct fixture_io ops = io(); assert(!fixture_init(&f, &ops) && f.fault);
}
static void test_tx_and_wire_completion(void)
{
    init(); cmd("V1 10 TX_PATTERN 260 254 5000"); hw.capacity = 3;
    fixture_tick(&f, 100); assert(f.progress == 3 && hw.tx_len == 3);
    assert(hw.tx[0] == 254 && hw.tx[1] == 255 && hw.tx[2] == 0);
    for (unsigned i = 0; i < 100; ++i) { hw.capacity = 3; fixture_tick(&f, 101+i); }
    assert(f.progress == 260 && f.job == F_TX && f.tx_queued == 260);
    for (unsigned i = 0; i < 260; ++i) assert(hw.tx[i] == (uint8_t)(254+i));
    cmd("V1 11 CONFIG 9600 8 N 1 NONE"); contains("V1 11 ERR BUSY");
    cmd("V1 12 ECHO 1 5000"); contains("V1 12 ERR BUSY");
    hw.wire_busy = false; fixture_tick(&f, 210); fixture_tick(&f, 211);
    assert(f.job == F_IDLE); contains("DONE job=10 kind=TX result=OK count=260");
    clear_output(); fixture_tick(&f, 212); assert(!hw.output_len);
    cmd("V1 13 STATUS"); contains("last:job=10 kind=TX result=OK");
}
static void test_receive(void)
{
    init(); cmd("V1 1 RX_VERIFY 256 0 5000");
    uint8_t data[257];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    fixture_receive(&f, data, 257); fixture_tick(&f, 101); fixture_tick(&f, 102);
    assert(f.job == F_IDLE && f.rx_bytes == 257 && f.unexpected == 1);
    contains("result=OK count=256 mismatches=0 first_bad=4294967295");
    cmd("V1 2 RX_VERIFY 4 0 5000");
    data[1] = 9; data[3] = 9; fixture_receive(&f, data, 4);
    fixture_tick(&f, 101); fixture_tick(&f, 102);
    contains("job=2 kind=RX result=MISMATCH count=4 mismatches=2 first_bad=1");
    init(); cmd("V1 3 ECHO 256 5000"); hw.capacity = 256;
    for (unsigned i = 0; i < 256; ++i) data[i] = (uint8_t)i;
    fixture_receive(&f, data, 256); assert(f.job == F_ECHO && hw.tx_len == 256);
    assert(!memcmp(data, hw.tx, 256));
    fixture_tick(&f, 101); assert(f.job == F_ECHO);
    hw.wire_busy = false; fixture_tick(&f, 102); fixture_tick(&f, 103);
    contains("kind=ECHO result=OK count=256");
    init(); cmd("V1 4 ECHO 5 5000"); hw.capacity = 2;
    fixture_receive(&f, data, 5); fixture_tick(&f, 101);
    contains("result=ECHO_OVERFLOW count=2"); assert(hw.reset_calls == 2);
}
static void test_flow_and_patterns(void)
{
    init(); cmd("V1 1 CONFIG 9600 7 E 2 XONXOFF");
    bool seen[128] = {0};
    for (unsigned i = 0; i < 126; ++i) {
        uint8_t b = fixture_pattern(&f.config, 0, i);
        assert(b < 128 && b != 0x11 && b != 0x13 && !seen[b]); seen[b] = true;
    }
    assert(fixture_pattern(&f.config, 0, 126) == 0);
    cmd("V1 2 RX_VERIFY 126 0 5000");
    uint8_t off = 0x13, on = 0x11;
    fixture_receive(&f, &off, 1); assert(hw.paused && !f.progress);
    uint8_t data[126];
    for (unsigned i = 0; i < 126; ++i) data[i] = fixture_pattern(&f.config, 0, i);
    fixture_receive(&f, data, 126); fixture_receive(&f, &on, 1); assert(!hw.paused);
    fixture_tick(&f, 101); fixture_tick(&f, 102); contains("result=OK count=126");
    /* 8-bit software-flow alphabet also excludes exactly two symbols. */
    cmd("V1 3 CONFIG 115200 8 N 1 XONXOFF");
    bool seen8[256] = {0};
    for (unsigned i = 0; i < 254; ++i) {
        uint8_t b = fixture_pattern(&f.config, 255, i);
        assert(b != 0x11 && b != 0x13 && !seen8[b]); seen8[b] = true;
    }
    cmd("V1 3 CONFIG 115200 8 N 1 RTS_GATE"); assert(hw.paused);
    hw.inputs = F_RTS; fixture_tick(&f, 103); assert(!hw.paused);
    hw.inputs = F_DTR; fixture_tick(&f, 104); assert(hw.paused);
    cmd("V1 4 CONFIG 115200 8 N 1 NONE"); assert(!hw.paused);
    /* NONE treats XON/XOFF as ordinary binary payload, including for echo. */
    cmd("V1 5 ECHO 2 5000"); hw.capacity = 2;
    fixture_receive(&f, &off, 1); fixture_receive(&f, &on, 1);
    assert(hw.tx_len == 2 && hw.tx[0] == off && hw.tx[1] == on);
}
static void test_recovery_and_backpressure(void)
{
    init(); cmd("V1 1 TX_PATTERN 100 0 20"); hw.capacity = 0;
    fixture_tick(&f, 119); assert(f.job == F_TX);
    fixture_tick(&f, 120); assert(f.job == F_IDLE && hw.reset_calls == 2);
    contains("result=TIMEOUT");
    cmd("V1 2 SET_LINES 1 1 1 1"); cmd("V1 3 ECHO 3 5000");
    cmd("V1 4 STOP"); fixture_tick(&f, 121);
    contains("V1 4 OK stopped=1"); contains("job=3 kind=ECHO result=CANCELLED");
    assert(!hw.outputs && !f.xoff);
    cmd("V1 5 RX_VERIFY 2 0 5000"); fixture_uart_error(&f, 2);
    fixture_tick(&f, 122); contains("result=UART_ERROR"); assert(f.errors == 2);
    cmd("V1 6 RX_VERIFY 2 0 5000"); fixture_rx_overflow(&f, 17);
    fixture_tick(&f, 123); contains("result=RX_OVERFLOW"); assert(f.rx_dropped == 17);
    /* A result waits for output room. Neither ACK nor completion can be lost. */
    init(); hw.blocked = true; cmd("V1 7 TX_PATTERN 10 0 20");
    assert(!fixture_ready(&f)); fixture_tick(&f, 120);
    assert(f.job == F_IDLE && hw.reset_calls == 2); /* Timeout despite blocked ACK. */
    hw.blocked = false; assert(fixture_ready(&f));
    contains("V1 7 OK started=TX"); contains("V1 0 DONE job=7 kind=TX result=TIMEOUT");
    clear_output(); assert(fixture_ready(&f)); assert(!hw.output_len);
    cmd("V1 8 TX_PATTERN 1 0 5000"); fixture_tick(&f, 101);
    hw.wire_busy = false; fixture_tick(&f, 102);
    /* An immediately following command must not overwrite the pending DONE. */
    cmd("V1 9 RX_VERIFY 1 0 5000"); contains("DONE job=8 kind=TX result=OK");
    uint8_t b = 0; fixture_receive(&f, &b, 1); fixture_uart_error(&f, 1);
    fixture_tick(&f, 103); contains("job=9 kind=RX result=UART_ERROR");
    /* Worst-case STATUS formatting stays within its bounded line. */
    f.rx_bytes = f.tx_queued = f.unexpected = f.errors = f.rx_dropped = f.control_lost = UINT64_MAX;
    cmd("V1 4294967295 STATUS"); assert(!f.fault); contains("control_lost=18446744073709551615");
}
static void test_parser_stress(void)
{
    init();
    uint32_t r = 0x5a17;
    for (unsigned int i = 0; i < 20000; ++i) {
        clear_output();
        r = r * 1664525U + 1013904223U;
        unsigned int len = r % (FIXTURE_LINE_SIZE + 100);
        for (unsigned int j = 0; j < len; ++j) {
            r = r * 1664525U + 1013904223U;
            fixture_control(&f, (uint8_t)(r >> 24), i);
        }
        fixture_control(&f, '\n', i);
        assert(f.line_len == 0 && !f.discarding && f.job == F_IDLE && !f.fault);
        cmd("V1 99 GET_LINES"); contains("V1 99 OK");
    }
}
int main(void)
{
    test_protocol(); test_config_and_lines(); test_tx_and_wire_completion();
    test_receive(); test_flow_and_patterns(); test_recovery_and_backpressure(); test_parser_stress();
    puts("RS232 fixture: protocol, formats, lines, jobs, flow, errors and recovery passed");
    return 0;
}
