/* SPDX-License-Identifier: MIT */
#include "fixture.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *kind(enum fixture_job j)
{
    return j == F_TX ? "TX" : j == F_RX ? "RX" : j == F_ECHO ? "ECHO" : "IDLE";
}
static const char *flow_name(enum fixture_flow flow)
{
    return flow == F_XONXOFF ? "XONXOFF" : flow == F_RTS_GATE ? "RTS_GATE" : "NONE";
}
static void reply(struct fixture *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(f->pending, sizeof(f->pending) - 1, fmt, ap);
    va_end(ap);
    /* All reply formats are statically bounded below FIXTURE_LINE_SIZE. */
    if (n < 0 || (size_t)n >= sizeof(f->pending) - 1) {
        strcpy(f->pending, "V1 0 ERR INTERNAL");
        n = (int)strlen(f->pending);
        f->fault = true;
    }
    f->pending[n++] = '\n';
    f->pending_len = (size_t)n;
    (void)fixture_ready(f);
}
bool fixture_ready(struct fixture *f)
{
    if (f->pending_len && f->io.emit(f->io.ctx, f->pending, f->pending_len))
        f->pending_len = 0;
    if (!f->pending_len && f->completion_pending) {
        int n = snprintf(f->pending, sizeof(f->pending), "V1 0 DONE %s\n", f->last);
        f->pending_len = (size_t)n;
        f->completion_pending = false;
        if (f->io.emit(f->io.ctx, f->pending, f->pending_len)) f->pending_len = 0;
    }
    return f->pending_len == 0;
}
static void gate(struct fixture *f)
{
    bool paused = (f->config.flow == F_XONXOFF && f->xoff) ||
        (f->config.flow == F_RTS_GATE && !(f->io.inputs(f->io.ctx) & F_RTS));
    f->io.pause(f->io.ctx, paused);
}
uint8_t fixture_pattern(const struct fixture_config *c, uint32_t seed, uint32_t offset)
{
    uint32_t alphabet = c->bits == 7 ? 128 : 256;
    if (c->flow == F_XONXOFF) {
        uint32_t v = (seed + offset) % (alphabet - 2);
        if (v >= 0x11) ++v;
        if (v >= 0x13) ++v;
        return (uint8_t)v;
    }
    return (uint8_t)((seed + offset) % alphabet);
}
bool fixture_init(struct fixture *f, const struct fixture_io *io)
{
    memset(f, 0, sizeof(*f));
    f->io = *io;
    f->config = (struct fixture_config){115200, 8, 1, 'N', F_NONE};
    strcpy(f->last, "NONE");
    f->fault = io->configure(io->ctx, &f->config) != 0;
    if (io->set_lines(io->ctx, 0) != 0) f->fault = true;
    io->pause(io->ctx, false);
    return !f->fault;
}
static void finish(struct fixture *f, const char *result, bool abort)
{
    if (abort) f->io.reset(f->io.ctx);
    snprintf(f->last, sizeof(f->last),
        "job=%" PRIu32 " kind=%s result=%s count=%" PRIu32
        " mismatches=%" PRIu32 " first_bad=%" PRIu32,
        f->job_seq, kind(f->job), result, f->progress, f->mismatches, f->first_bad);
    f->job = F_IDLE;
    f->completion_pending = true;
    /* Defer completion notification until the command reply is transmitted. */
}
static bool number(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || v > (UINT32_MAX - (uint32_t)(*s - '0')) / 10)
            return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}
static bool baud_valid(uint32_t b)
{
    static const uint32_t rates[] = {1200,2400,4800,9600,19200,38400,57600,115200,230400};
    for (size_t i = 0; i < sizeof(rates)/sizeof(rates[0]); ++i)
        if (rates[i] == b) return true;
    return false;
}
static void command(struct fixture *f, uint64_t now)
{
    char *a[10];
    size_t n = 0;
    char *s = f->line;
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (n == sizeof(a)/sizeof(a[0])) { n = 0; break; }
        a[n++] = s;
        while (*s && *s != ' ') ++s;
        if (*s) *s++ = 0;
    }
    uint32_t seq = 0, v[5] = {0};
    if (n < 3 || strcmp(a[0], "V1") || !number(a[1], &seq) || !seq) {
        reply(f, "V1 0 ERR SYNTAX"); return;
    }
#define IS(c) (!strcmp(a[2], (c)))
#define ERR(e) do { reply(f, "V1 %" PRIu32 " ERR " e, seq); return; } while (0)
    if (IS("CAPS") && n == 3) {
        reply(f, "V1 %" PRIu32 " OK version=1 board=frdm_mcxn947 uart=LPUART6 "
            "baud=1200,2400,4800,9600,19200,38400,57600,115200,230400 "
            "bits=7,8 parity=N,O,E stops=1,2 flow=NONE,XONXOFF,RTS_GATE "
            "break=0 mark_space=0 native_rtscts=0 max_count=%u", seq, FIXTURE_MAX_COUNT);
    } else if (IS("GET_LINES") && n == 3) {
        if (f->fault) ERR("FAULT");
        uint8_t m = f->io.inputs(f->io.ctx) | f->outputs;
        reply(f, "V1 %" PRIu32 " OK cts=%u dsr=%u dcd=%u ri=%u rts=%u dtr=%u",
            seq, !!(m & F_CTS), !!(m & F_DSR), !!(m & F_DCD), !!(m & F_RI),
            !!(m & F_RTS), !!(m & F_DTR));
    } else if (IS("STATUS") && n == 3) {
        reply(f, "V1 %" PRIu32 " OK state=%s fault=%u progress=%" PRIu32
            " baud=%" PRIu32 " bits=%u parity=%c stops=%u flow=%s"
            " rx=%" PRIu64 " tx_queued=%" PRIu64 " unexpected=%" PRIu64
            " uart_errors=%" PRIu64 " rx_dropped=%" PRIu64 " control_lost=%" PRIu64 " last:%s",
            seq, kind(f->job), f->fault, f->progress, f->config.baud, f->config.bits,
            f->config.parity, f->config.stops, flow_name(f->config.flow), f->rx_bytes, f->tx_queued,
            f->unexpected, f->errors, f->rx_dropped, f->control_lost, f->last);
    } else if (IS("STOP") && n == 3) {
        if (f->job != F_IDLE) finish(f, "CANCELLED", true);
        else f->io.reset(f->io.ctx);
        f->xoff = false;
        int e = f->io.configure(f->io.ctx, &f->config);
        if (f->io.set_lines(f->io.ctx, 0) != 0) e = -1;
        f->outputs = 0;
        f->fault = e != 0;
        gate(f);
        if (f->fault) ERR("IO");
        reply(f, "V1 %" PRIu32 " OK stopped=1", seq);
    } else if (IS("SET_LINES")) {
        if (n != 7) ERR("ARGS");
        for (size_t i = 0; i < 4; ++i)
            if (!number(a[i+3], &v[i]) || v[i] > 1) ERR("ARGS");
        uint8_t m = (uint8_t)(v[0] | v[1]<<1 | v[2]<<2 | v[3]<<3);
        if (f->io.set_lines(f->io.ctx, m)) {
            f->fault = true;
            if (f->job != F_IDLE) finish(f, "IO_ERROR", true);
            ERR("IO");
        }
        f->outputs = m;
        reply(f, "V1 %" PRIu32 " OK", seq);
    } else if (IS("CONFIG")) {
        if (n != 8 || !number(a[3], &v[0]) || !number(a[4], &v[1]) ||
            !number(a[6], &v[2])) ERR("ARGS");
        struct fixture_config c = f->config;
        if (!baud_valid(v[0]) || (v[1] != 7 && v[1] != 8) ||
            (v[2] != 1 && v[2] != 2) || strlen(a[5]) != 1 ||
            !strchr("NOE", a[5][0])) ERR("UNSUPPORTED");
        if (!strcmp(a[7], "NONE")) c.flow = F_NONE;
        else if (!strcmp(a[7], "XONXOFF")) c.flow = F_XONXOFF;
        else if (!strcmp(a[7], "RTS_GATE")) c.flow = F_RTS_GATE;
        else ERR("UNSUPPORTED");
        if (f->job != F_IDLE || !f->io.tx_idle(f->io.ctx)) ERR("BUSY");
        if (f->fault) ERR("FAULT");
        c.baud = v[0]; c.bits = (uint8_t)v[1]; c.parity = a[5][0]; c.stops = (uint8_t)v[2];
        if (f->io.configure(f->io.ctx, &c)) {
            /* Some drivers partially disable the UART on failed configure. */
            if (f->io.configure(f->io.ctx, &f->config)) f->fault = true;
            ERR("IO");
        }
        f->config = c; f->xoff = false; gate(f);
        reply(f, "V1 %" PRIu32 " OK", seq);
    } else if (IS("TX_PATTERN") || IS("RX_VERIFY") || IS("ECHO")) {
        bool echo = IS("ECHO");
        size_t args = echo ? 5 : 6;
        if (n != args || !number(a[3], &v[0]) || !v[0] || v[0] > FIXTURE_MAX_COUNT ||
            (!echo && (!number(a[4], &v[1]) || v[1] > 255)) ||
            !number(a[args-1], &v[2]) || !v[2] || v[2] > 3600000) ERR("ARGS");
        if (f->fault) ERR("FAULT");
        if (f->job != F_IDLE || !f->io.tx_idle(f->io.ctx)) ERR("BUSY");
        f->io.reset(f->io.ctx);
        f->job = echo ? F_ECHO : IS("TX_PATTERN") ? F_TX : F_RX;
        f->job_seq = seq; f->goal = v[0]; f->seed = v[1]; f->progress = 0;
        f->mismatches = 0; f->first_bad = UINT32_MAX; f->deadline = now + v[2];
        reply(f, "V1 %" PRIu32 " OK started=%s count=%" PRIu32, seq, kind(f->job), f->goal);
    } else ERR("COMMAND");
#undef ERR
#undef IS
}
void fixture_control(struct fixture *f, uint8_t b, uint64_t now)
{
    /* Caller applies output backpressure; no partial response may be replaced. */
    if (!fixture_ready(f)) return;
    if (b == '\n') {
        if (f->discarding) {
            f->discarding = false; f->line_len = 0;
            reply(f, "V1 0 ERR CONTROL_LINE"); return;
        }
        if (f->line_len && f->line[f->line_len-1] == '\r') --f->line_len;
        f->line[f->line_len] = 0;
        command(f, now); f->line_len = 0;
    } else if (!f->discarding) {
        if (b < 32 && b != '\r') f->discarding = true;
        else if (b > 126 || f->line_len == sizeof(f->line)-1) f->discarding = true;
        else f->line[f->line_len++] = (char)b;
    }
}
void fixture_control_overflow(struct fixture *f, uint32_t dropped)
{
    f->control_lost += dropped;
    f->line_len = 0; f->discarding = true;
}
void fixture_receive(struct fixture *f, const uint8_t *data, size_t len)
{
    f->rx_bytes += len;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (f->config.flow == F_XONXOFF && (b == 0x11 || b == 0x13)) {
            f->xoff = b == 0x13; gate(f); continue;
        }
        if (f->job == F_RX && f->progress < f->goal) {
            if (b != fixture_pattern(&f->config, f->seed, f->progress)) {
                if (!f->mismatches) f->first_bad = f->progress;
                ++f->mismatches;
            }
            ++f->progress;
        } else if (f->job == F_ECHO && f->progress < f->goal) {
            if (f->io.write(f->io.ctx, &b, 1) != 1) {
                finish(f, "ECHO_OVERFLOW", true); continue;
            }
            ++f->progress; ++f->tx_queued;
        } else ++f->unexpected;
    }
}
void fixture_rx_overflow(struct fixture *f, uint32_t dropped)
{
    f->rx_dropped += dropped;
    if (dropped && f->job != F_IDLE) finish(f, "RX_OVERFLOW", true);
}
void fixture_uart_error(struct fixture *f, uint32_t observations)
{
    f->errors += observations;
    if (observations && f->job != F_IDLE) finish(f, "UART_ERROR", true);
}
void fixture_tick(struct fixture *f, uint64_t now)
{
    gate(f);
    /* A blocked control reader must not suppress the data-path watchdog. */
    if (f->job != F_IDLE && now >= f->deadline) finish(f, "TIMEOUT", true);
    if (!fixture_ready(f) || f->job == F_IDLE) return;
    if (f->job == F_RX && f->progress == f->goal) {
        finish(f, f->mismatches ? "MISMATCH" : "OK", false);
        return;
    }
    if (f->job == F_TX && f->progress < f->goal) {
        uint8_t b[128];
        size_t n = f->goal - f->progress;
        if (n > sizeof(b)) n = sizeof(b);
        for (size_t i = 0; i < n; ++i) b[i] = fixture_pattern(&f->config, f->seed, f->progress + (uint32_t)i);
        size_t sent = f->io.write(f->io.ctx, b, n);
        f->progress += (uint32_t)sent; f->tx_queued += sent;
    }
    if ((f->job == F_TX || f->job == F_ECHO) && f->progress == f->goal && f->io.tx_idle(f->io.ctx))
        finish(f, "OK", false);
}
