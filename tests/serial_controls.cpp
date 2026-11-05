// Software contract tests: a real PTY supplies transport/lifecycle behavior;
// only modem/BREAK/counter ioctls are replaced at the linker boundary. These
// tests do not claim that a PTY has RS-232 pins or validates electrical levels.
#include <astrial.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <termios.h>
#include <future>
#include <iostream>
#include <linux/serial.h>
#include <mutex>
#include <pty.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace
{
struct NativeDriver
{
    std::mutex mutex;
    int bits{};
    int output_error{};
    int query_error{};
    int break_error{};
    int counter_error{};
    int queries{};
    int output_attempts{};
    bool parity_mock{};
    termios parity_state{};
    bool breaking{};
    std::vector<std::pair<int, bool>> outputs;
    std::vector<bool> breaks;
    void reset()
    {
        std::lock_guard lock(mutex);
        bits = output_error = query_error = break_error = counter_error = queries = 0;
        output_attempts = 0;
        breaking = false;
        outputs.clear();
        breaks.clear();
    }
} driver;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T ready(std::future<T>& future)
{
    require(future.wait_for(2s) == std::future_status::ready, "callback timed out");
    return future.get();
}

struct Terminal
{
    int master{-1};
    std::string name;
    Terminal()
    {
        char path[128]{};
        int slave{};
        if (::openpty(&master, &slave, path, nullptr, nullptr))
            throw std::system_error(errno, std::generic_category());
        ::close(slave);
        name = path;
    }
    void disconnect() { if (master >= 0) ::close(std::exchange(master, -1)); }
    ~Terminal() { disconnect(); }
};

Serial open(Terminal& terminal)
{
    return Serial::builder().baud_rate(115200).auto_reconnect(false).open(terminal.name).value();
}

void test_outputs_and_error_mapping()
{
    driver.reset();
    Terminal terminal;
    auto serial = open(terminal);
    require(serial.set_data_terminal_ready(true).has_value(), "DTR set failed");
    require(serial.set_request_to_send(true).has_value(), "RTS set failed");
    require(serial.set_data_terminal_ready(false).has_value(), "DTR clear failed");
    require(!serial.is_data_terminal_ready().value(), "DTR getter wrong");
    require(serial.is_request_to_send().value(), "DTR clear overwrote RTS");
    {
        std::lock_guard lock(driver.mutex);
        driver.bits |= TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RNG;
    }
    const auto pins = serial.pinout_signals().value();
    for (auto bit : {PinoutSignals::ClearToSend, PinoutSignals::DataSetReady,
                     PinoutSignals::DataCarrierDetect, PinoutSignals::RingIndicator})
        require(has_signal(pins, bit), "pinout input mapping wrong");
    require(!has_signal(pins, PinoutSignals::None), "None is not an asserted signal");
    {
        std::lock_guard lock(driver.mutex);
        driver.output_error = ENOTTY;
    }
    auto unsupported = serial.set_data_terminal_ready(true);
    require(!unsupported && unsupported.error() == SerialError::UnsupportedOperation,
            "unsupported ioctl was not normalized");
    require(!serial.is_data_terminal_ready().value(), "failed output changed state");
    {
        std::lock_guard lock(driver.mutex);
        driver.query_error = EIO;
    }
    auto failed = serial.pinout_signals();
    require(!failed && failed.error() == std::error_code(EIO, std::generic_category()),
            "native query error was not preserved");
    serial.close();
}

void test_hardware_flow_ownership_and_builder()
{
    driver.reset();
    Terminal terminal;
    auto built = Serial::builder().baud_rate(115200).data_terminal_ready(false)
                     .request_to_send(true).auto_reconnect(false).open(terminal.name);
    require(built.has_value(), "builder output setup failed");
    auto serial = std::move(*built);
    require(!serial.is_data_terminal_ready().value() && serial.is_request_to_send().value(),
            "builder outputs not applied");
    require(serial.set_flow_control(FlowControl::Hardware).has_value(), "hardware flow failed");
    int calls;
    { std::lock_guard lock(driver.mutex); calls = driver.outputs.size(); }
    auto blocked = serial.set_request_to_send(false);
    require(!blocked && blocked.error() == SerialError::UnsupportedOperation,
            "manual RTS allowed in hardware mode");
    { std::lock_guard lock(driver.mutex);
      require(driver.outputs.size() == static_cast<size_t>(calls), "blocked RTS touched native driver"); }
    require(serial.set_data_terminal_ready(true).has_value(), "hardware flow wrongly blocked DTR");
    require(serial.set_flow_control(FlowControl::None).has_value(), "flow None failed");
    require(serial.set_request_to_send(false).has_value(), "manual RTS not restored after flow None");
    serial.close();
    auto conflict = Serial::builder().baud_rate(115200).flow_control(FlowControl::Hardware)
                        .request_to_send(false).open(terminal.name);
    require(!conflict && conflict.error() == SerialError::UnsupportedOperation,
            "builder accepted explicit RTS plus hardware flow");
    { std::lock_guard lock(driver.mutex); driver.output_error = EIO; }
    auto failed = Serial::builder().baud_rate(115200).data_terminal_ready(true).open(terminal.name);
    require(!failed, "builder ignored control setup failure");
}

void test_break_timer_and_cancellation()
{
    driver.reset();
    Terminal terminal;
    auto serial = open(terminal);
    require(serial.set_break_enabled(true).has_value(), "persistent BREAK failed");
    require(serial.is_break_enabled().value(), "BREAK state not retained");
    auto conflict = serial.send_break(5ms);
    require(!conflict && conflict.error() == std::errc::operation_in_progress,
            "pulse overwrote persistent BREAK");
    require(serial.set_break_enabled(false).has_value(), "BREAK clear failed");
    auto invalid = serial.send_break(0ms);
    require(!invalid && invalid.error() == SerialError::InvalidArgument, "zero pulse accepted");
    { std::lock_guard lock(driver.mutex); driver.break_error = ENOTTY; }
    auto unsupported = serial.set_break_enabled(true);
    require(!unsupported && unsupported.error() == SerialError::UnsupportedOperation,
            "BREAK failure hidden");
    require(!serial.is_break_enabled().value(), "failed BREAK changed cache");
    { std::lock_guard lock(driver.mutex); driver.break_error = 0; }
    std::promise<std::error_code> completion;
    auto done = completion.get_future();
    std::atomic<int> calls{};
    serial.async_send_break(80ms, [&](const std::error_code& error)
    {
        ++calls;
        completion.set_value(error);
    });
    require(serial.is_break_enabled().value(), "pulse did not assert BREAK");
    require(serial.set_data_terminal_ready(true).has_value(), "pulse blocked the I/O thread");
    auto busy = serial.set_break_enabled(false);
    require(!busy && busy.error() == std::errc::operation_in_progress, "pulse allowed manual override");
    require(!ready(done), "pulse failed");
    require(calls == 1 && !serial.is_break_enabled().value(), "pulse not completed once/cleared");
    require(serial.send_break(5ms).has_value(), "synchronous pulse failed");

    std::promise<std::error_code> cancellation;
    auto cancelled = cancellation.get_future();
    serial.async_send_break(5s, [&](const std::error_code& error) { cancellation.set_value(error); });
    require(serial.is_break_enabled().value(), "cancel test did not start");
    serial.close();
    require(static_cast<bool>(ready(cancelled)), "close did not cancel pulse");
    { std::lock_guard lock(driver.mutex); require(!driver.breaking, "close left BREAK asserted"); }
}

void test_callback_reentrancy_move_and_closed()
{
    driver.reset();
    Terminal terminal;
    auto old = open(terminal);
    auto serial = std::move(old);
    require(!old.pinout_signals() && !old.set_request_to_send(true) && !old.is_break_enabled(),
            "moved-from control must fail safely");
    std::promise<bool> checked;
    auto done = checked.get_future();
    serial.async_send_break(2ms, [&](const std::error_code& error)
    {
        const auto deadlock = serial.send_break(1ms);
        const bool valid = !error && serial.pinout_signals().has_value() &&
                           serial.set_data_terminal_ready(true).has_value() && !deadlock &&
                           deadlock.error() == std::errc::resource_deadlock_would_occur;
        serial.close();
        checked.set_value(valid);
    });
    require(ready(done), "callback controls deadlocked or failed");
    serial.close(); // join after callback-initiated close
    require(!serial.pinout_signals() && !serial.is_data_terminal_ready() &&
            !serial.is_request_to_send() && !serial.set_break_enabled(true) &&
            !serial.is_break_enabled() && !serial.line_error_counters() &&
            !serial.on_pinout_changed([](auto&, auto) {}), "closed controls succeeded");
}

void test_pinout_monitor()
{
    driver.reset();
    Terminal terminal;
    auto serial = open(terminal);
    std::atomic<int> changes{};
    std::promise<bool> changed;
    auto done = changed.get_future();
    auto invalid = serial.on_pinout_changed([](auto&, auto) {}, 0ms);
    require(!invalid && invalid.error() == SerialError::InvalidArgument, "zero interval accepted");
    require(serial.on_pinout_changed([&](const std::error_code& error, PinoutSignals pins)
    {
        ++changes;
        // Self-unregister and synchronously query on the I/O thread.
        const bool okay = !error && has_signal(pins, PinoutSignals::ClearToSend) &&
                          serial.on_pinout_changed({}).has_value() && serial.pinout_signals().has_value();
        changed.set_value(okay);
    }, 2ms).has_value(), "monitor registration failed");
    std::this_thread::sleep_for(10ms);
    require(changes == 0, "unchanged baseline emitted a callback");
    { std::lock_guard lock(driver.mutex); driver.bits |= TIOCM_CTS; }
    require(ready(done), "monitor missed change or self-unregister failed");
    std::this_thread::sleep_for(10ms);
    require(changes == 1, "disabled monitor still notified");

    std::promise<std::error_code> failed;
    auto failure = failed.get_future();
    require(serial.on_pinout_changed([&](const std::error_code& error, PinoutSignals)
    {
        ++changes;
        failed.set_value(error);
    }, 2ms).has_value(), "monitor re-registration failed");
    { std::lock_guard lock(driver.mutex); driver.query_error = EIO; }
    require(ready(failure) == std::error_code(EIO, std::generic_category()), "monitor lost error");
    std::this_thread::sleep_for(10ms);
    require(changes == 2, "monitor repeated the same error");
    serial.close();
}

void test_parity_option_encoding()
{
    driver.reset();
    Terminal terminal;
    auto serial = open(terminal);
    { std::lock_guard lock(driver.mutex); driver.parity_mock = true; driver.parity_state = {}; }
    struct Reset { ~Reset() { std::lock_guard lock(driver.mutex); driver.parity_mock = false; } } reset;
#ifdef CMSPAR
    require(serial.set_parity(Parity::Mark).has_value(), "Mark setup failed");
    { std::lock_guard lock(driver.mutex);
      require((driver.parity_state.c_cflag & (CMSPAR | PARODD | PARENB)) ==
              (CMSPAR | PARODD | PARENB), "Mark flags wrong"); }
    require(serial.set_parity(Parity::Space).has_value(), "Space setup failed");
    { std::lock_guard lock(driver.mutex);
      require((driver.parity_state.c_cflag & (CMSPAR | PARENB)) == (CMSPAR | PARENB) &&
              !(driver.parity_state.c_cflag & PARODD), "Space flags wrong"); }
#endif
    require(serial.set_parity(Parity::Odd).has_value(), "Odd setup failed");
    { std::lock_guard lock(driver.mutex);
      require((driver.parity_state.c_cflag & (PARENB | PARODD)) == (PARENB | PARODD), "Odd flags wrong");
#ifdef CMSPAR
      require(!(driver.parity_state.c_cflag & CMSPAR), "Mark/Space leaked into Odd");
#endif
    }
    require(serial.set_parity(Parity::Even).has_value(), "Even setup failed");
    { std::lock_guard lock(driver.mutex);
      require((driver.parity_state.c_cflag & PARENB) && !(driver.parity_state.c_cflag & PARODD),
              "Even flags wrong"); }
    require(serial.set_parity(Parity::None).has_value(), "None setup failed");
    { std::lock_guard lock(driver.mutex);
      require(!(driver.parity_state.c_cflag & (PARENB | PARODD)), "None retained parity flags"); }
    const auto invalid = serial.set_parity(static_cast<Parity>(99));
    require(!invalid && invalid.error() == SerialError::InvalidArgument, "invalid parity silently accepted");
    serial.close();
}

void test_break_clear_failure_and_command_close_race()
{
    driver.reset();
    Terminal terminal;
    auto serial = open(terminal);
    std::promise<std::error_code> completion;
    auto done = completion.get_future();
    serial.async_send_break(10ms, [&](const auto& error) { completion.set_value(error); });
    require(serial.is_break_enabled().value(), "pulse not started");
    { std::lock_guard lock(driver.mutex); driver.break_error = EIO; }
    require(ready(done) == std::error_code(EIO, std::generic_category()), "BREAK clear error lost");
    require(serial.is_break_enabled().value(), "failed clear falsely reported BREAK disabled");
    { std::lock_guard lock(driver.mutex); driver.break_error = 0; }
    require(serial.set_break_enabled(false).has_value(), "BREAK clear retry failed");
    std::promise<void> started;
    auto first = started.get_future();
    std::jthread worker([&]
    {
        (void)serial.pinout_signals();
        started.set_value();
        while (serial.pinout_signals()) {}
    });
    require(first.wait_for(2s) == std::future_status::ready, "control worker did not start");
    serial.close();
    worker.join();
}

void test_counter_contract()
{
    driver.reset();
    Terminal terminal;
    auto serial = open(terminal);
    auto counters = serial.line_error_counters();
    require(counters && counters->parity == 1 && counters->framing == 2 &&
            counters->overrun == 3 && counters->breaks == 4 && counters->buffer_overrun == 5,
            "counter field mapping wrong");
    { std::lock_guard lock(driver.mutex); driver.counter_error = ENOTTY; }
    counters = serial.line_error_counters();
    require(!counters && counters.error() == SerialError::UnsupportedOperation,
            "unsupported counters synthesized zeros");
    serial.close();
}

void test_reconnect_restores_outputs_not_break()
{
    driver.reset();
    Terminal first;
    Terminal second;
    char dir_template[] = "/tmp/astrial-control-XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    require(dir != nullptr, "mkdtemp failed");
    const std::string directory(dir), link = directory + "/port";
    struct Cleanup { std::string link, dir; ~Cleanup() { ::unlink(link.c_str()); ::rmdir(dir.c_str()); } } cleanup{link, directory};
    require(::symlink(first.name.c_str(), link.c_str()) == 0, "symlink failed");
    auto serial = Serial::builder().baud_rate(115200).auto_reconnect(true, 10ms).open(link).value();
    require(serial.set_data_terminal_ready(false).has_value() && serial.set_request_to_send(true).has_value(),
            "reconnect output setup failed");
    // A failed setter must not overwrite saved DTR=false. Also force one
    // reconnect-restore failure, so retry must close the failed open handle.
    { std::lock_guard lock(driver.mutex); driver.output_error = EIO; }
    require(!serial.set_data_terminal_ready(true), "failure injection did not reject DTR");
    int attempts_before;
    { std::lock_guard lock(driver.mutex); attempts_before = driver.output_attempts; }
    serial.on_data([](auto) {});
    std::promise<std::error_code> broke;
    auto interrupted = broke.get_future();
    serial.async_send_break(5s, [&](const std::error_code& error) { broke.set_value(error); });
    require(serial.is_break_enabled().value(), "reconnect pulse not started");
    std::promise<bool> reconnected;
    auto connected = reconnected.get_future();
    serial.on_reconnect([&]
    {
        reconnected.set_value(!serial.is_data_terminal_ready().value() &&
                              serial.is_request_to_send().value() && !serial.is_break_enabled().value());
    });
    std::atomic<bool> watch_reconnect{false};
    std::promise<bool> resumed;
    auto monitoring = resumed.get_future();
    require(serial.on_pinout_changed([&](const auto& error, PinoutSignals pins)
    {
        if (watch_reconnect && has_signal(pins, PinoutSignals::RingIndicator))
            resumed.set_value(!error);
    }, 5ms).has_value(), "reconnect monitor setup failed");
    ::unlink(link.c_str());
    require(::symlink(second.name.c_str(), link.c_str()) == 0, "replacement symlink failed");
    // Simulate new driver defaults; successful reconnect must reapply even false.
    { std::lock_guard lock(driver.mutex); driver.bits = TIOCM_DTR; }
    first.disconnect();
    require(static_cast<bool>(ready(interrupted)), "disconnect did not cancel BREAK");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool attempted = false;
    while (!attempted && std::chrono::steady_clock::now() < deadline)
    {
        { std::lock_guard lock(driver.mutex); attempted = driver.output_attempts > attempts_before; }
        if (!attempted) std::this_thread::sleep_for(1ms);
    }
    require(attempted, "reconnect did not attempt output restoration");
    require(serial.state() == SerialState::Reconnecting, "failed restore reported Connected");
    { std::lock_guard lock(driver.mutex); driver.output_error = 0; }
    require(ready(connected), "reconnect did not restore outputs or replayed BREAK");
    watch_reconnect = true;
    { std::lock_guard lock(driver.mutex); driver.bits |= TIOCM_RNG; }
    require(ready(monitoring), "pin monitoring did not resume on reconnect");
    serial.close();
}
}

extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    if (request == TIOCSBRK || request == TIOCCBRK)
    {
        std::lock_guard lock(driver.mutex);
        if (driver.break_error) { errno = driver.break_error; return -1; }
        driver.breaking = request == TIOCSBRK;
        driver.breaks.push_back(driver.breaking);
        return 0;
    }
    va_list arguments;
    va_start(arguments, request);
    void* pointer = va_arg(arguments, void*);
    va_end(arguments);
    {
        std::lock_guard lock(driver.mutex);
        int error = 0;
        if (request == TIOCMGET)
        {
            ++driver.queries;
            error = driver.query_error;
            if (!error) *static_cast<int*>(pointer) = driver.bits;
        }
        else if (request == TIOCMBIS || request == TIOCMBIC)
        {
            ++driver.output_attempts;
            error = driver.output_error;
            if (!error)
            {
                const int bits = *static_cast<int*>(pointer);
                const bool asserted = request == TIOCMBIS;
                driver.outputs.emplace_back(bits, asserted);
                if (asserted) driver.bits |= bits; else driver.bits &= ~bits;
            }
        }
        else if (request == TIOCGICOUNT)
        {
            error = driver.counter_error;
            if (!error)
            {
                auto& counters = *static_cast<serial_icounter_struct*>(pointer);
                counters.parity = 1; counters.frame = 2; counters.overrun = 3;
                counters.brk = 4; counters.buf_overrun = 5;
            }
        }
        else return __real_ioctl(fd, request, pointer);
        if (error) { errno = error; return -1; }
    }
    return 0;
}

extern "C" int __real_tcgetattr(int, termios*);
extern "C" int __real_tcsetattr(int, int, const termios*);
extern "C" int __wrap_tcgetattr(int fd, termios* state)
{
    std::lock_guard lock(driver.mutex);
    if (!driver.parity_mock) return __real_tcgetattr(fd, state);
    *state = driver.parity_state;
    return 0;
}
extern "C" int __wrap_tcsetattr(int fd, int action, const termios* state)
{
    std::lock_guard lock(driver.mutex);
    if (!driver.parity_mock) return __real_tcsetattr(fd, action, state);
    driver.parity_state = *state;
    return 0;
}

int main()
{
    try
    {
        test_outputs_and_error_mapping();
        test_hardware_flow_ownership_and_builder();
        test_break_timer_and_cancellation();
        test_callback_reentrancy_move_and_closed();
        test_pinout_monitor();
        test_counter_contract();
        test_parity_option_encoding();
        test_break_clear_failure_and_command_close_race();
        test_reconnect_restores_outputs_not_break();
        std::cout << "Serial control software contract tests passed (no electrical validation)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
