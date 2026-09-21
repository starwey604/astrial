#include <astrial.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
using namespace std::chrono_literals;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

class PseudoTerminal
{
public:
    PseudoTerminal()
    {
        int slave = -1;
        std::array<char, 256> name{};
        if (::openpty(&m_master, &slave, name.data(), nullptr, nullptr) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "openpty");
        }

        termios attributes{};
        if (::tcgetattr(slave, &attributes) != 0)
        {
            const int saved_errno = errno;
            ::close(slave);
            throw std::system_error(saved_errno, std::generic_category(), "tcgetattr");
        }
        ::cfmakeraw(&attributes);
        if (::tcsetattr(slave, TCSANOW, &attributes) != 0)
        {
            const int saved_errno = errno;
            ::close(slave);
            throw std::system_error(saved_errno, std::generic_category(), "tcsetattr");
        }

        m_slave_name = name.data();
        ::close(slave);
    }

    ~PseudoTerminal()
    {
        if (m_master >= 0) ::close(m_master);
    }

    PseudoTerminal(const PseudoTerminal&) = delete;
    PseudoTerminal& operator=(const PseudoTerminal&) = delete;

    [[nodiscard]] const std::string& slave_name() const { return m_slave_name; }

    void write_all(std::span<const uint8_t> bytes) const
    {
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const auto written = ::write(m_master, bytes.data() + offset, bytes.size() - offset);
            if (written < 0)
            {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "PTY write");
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    std::vector<uint8_t> read_exact(std::size_t length) const
    {
        std::vector<uint8_t> result(length);
        std::size_t offset = 0;
        while (offset < length)
        {
            pollfd descriptor{m_master, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 2000);
            if (ready == 0) throw std::runtime_error("timed out reading PTY master");
            if (ready < 0)
            {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "PTY poll");
            }

            const auto received = ::read(m_master, result.data() + offset, length - offset);
            if (received < 0)
            {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "PTY read");
            }
            offset += static_cast<std::size_t>(received);
        }
        return result;
    }

private:
    int m_master{-1};
    std::string m_slave_name;
};

Serial open_serial(const PseudoTerminal& terminal)
{
    auto opened = Serial::builder()
                  .baud_rate(115200)
                  .parity(Parity::None)
                  .stop_bits(StopBits::One)
                  .auto_reconnect(false)
                  .open(terminal.slave_name());
    if (!opened) throw std::system_error(opened.error(), "open virtual serial port");
    return std::move(opened.value());
}

#if defined(__linux__)
// Opens the PTY slave a second time to read back the line settings that the
// library applied to the same terminal. A Linux pseudo-terminal preserves the
// baud rate, stop bits, and flow-control flags, but its driver forces 8N1, so
// parity enable and character size are not observable through a PTY.
termios read_slave_termios(const PseudoTerminal& terminal)
{
    const int fd = ::open(terminal.slave_name().c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "open PTY slave");

    termios attributes{};
    if (::tcgetattr(fd, &attributes) != 0)
    {
        const int saved_errno = errno;
        ::close(fd);
        throw std::system_error(saved_errno, std::generic_category(), "tcgetattr PTY slave");
    }
    ::close(fd);
    return attributes;
}
#endif

template <typename Predicate>
void wait_for(std::condition_variable& cv, std::mutex& mutex, Predicate predicate,
              const std::string& message)
{
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, 2s, std::move(predicate))) throw std::runtime_error(message);
}

void test_borrowed_read_pause_resume_and_cancel()
{
    PseudoTerminal terminal;
    auto serial = open_serial(terminal);

    std::array<uint8_t, 64> buffer{};
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<unsigned int> credits{1};
    std::atomic<unsigned int> provider_calls{0};
    std::vector<uint8_t> received;
    std::error_code final_error;
    unsigned int completions = 0;

    serial.on_data_borrowed(
        [&]() -> std::span<uint8_t>
        {
            provider_calls.fetch_add(1, std::memory_order_relaxed);
            unsigned int available = credits.load(std::memory_order_acquire);
            while (available != 0)
            {
                if (credits.compare_exchange_weak(available, available - 1,
                                                  std::memory_order_acq_rel))
                {
                    cv.notify_all();
                    return buffer;
                }
            }
            cv.notify_all();
            return {};
        },
        [&](const std::error_code& error, std::size_t length)
        {
            std::lock_guard lock(mutex);
            received.insert(received.end(), buffer.begin(), buffer.begin() + length);
            final_error = error;
            ++completions;
            cv.notify_all();
        });

    static constexpr std::array<uint8_t, 5> first{1, 2, 3, 4, 5};
    terminal.write_all(first);
    wait_for(cv, mutex, [&] { return completions >= 1; }, "borrowed RX did not complete");

    static constexpr std::array<uint8_t, 3> second{6, 7, 8};
    terminal.write_all(second);
    std::this_thread::sleep_for(50ms);
    {
        std::lock_guard lock(mutex);
        require(completions == 1, "empty provider span did not pause RX");
    }

    credits.store(1, std::memory_order_release);
    serial.resume_read();
    wait_for(cv, mutex, [&] { return completions >= 2; }, "resume_read did not re-arm RX");

    {
        std::lock_guard lock(mutex);
        const std::vector<uint8_t> expected{1, 2, 3, 4, 5, 6, 7, 8};
        require(received == expected, "borrowed RX payload mismatch");
        require(!final_error, "successful borrowed RX reported an error");
    }

    credits.store(1, std::memory_order_release);
    serial.resume_read();
    wait_for(cv, mutex, [&] { return provider_calls.load(std::memory_order_acquire) >= 5; },
             "final borrowed RX buffer was not acquired");
    serial.close();

    {
        std::lock_guard lock(mutex);
        require(completions == 3, "close did not return the outstanding borrowed RX buffer");
        require(static_cast<bool>(final_error), "cancelled borrowed RX did not report an error");
    }
}

void test_borrowed_and_owned_writes()
{
    PseudoTerminal terminal;
    auto serial = open_serial(terminal);

    static constexpr std::array<uint8_t, 6> borrowed{9, 8, 7, 6, 5, 4};
    std::mutex mutex;
    std::condition_variable cv;
    bool borrowed_done = false;
    std::error_code borrowed_error;
    serial.async_write_borrowed(borrowed, [&](const std::error_code& error, std::size_t length)
    {
        std::lock_guard lock(mutex);
        borrowed_error = error;
        borrowed_done = length == borrowed.size();
        cv.notify_all();
    });

    require(terminal.read_exact(borrowed.size()) == std::vector<uint8_t>(borrowed.begin(), borrowed.end()),
            "borrowed TX payload mismatch");
    wait_for(cv, mutex, [&] { return borrowed_done; }, "borrowed TX callback did not complete");
    require(!borrowed_error, "borrowed TX reported an error");

    static constexpr std::array<uint8_t, 4> expected_owned{11, 12, 13, 14};
    bool owned_done = false;
    std::error_code owned_error;
    {
        std::vector<uint8_t> temporary(expected_owned.begin(), expected_owned.end());
        serial.async_write(temporary, [&](const std::error_code& error, std::size_t length)
        {
            std::lock_guard lock(mutex);
            owned_error = error;
            owned_done = length == expected_owned.size();
            cv.notify_all();
        });
        std::fill(temporary.begin(), temporary.end(), 0xff);
    }

    require(terminal.read_exact(expected_owned.size()) ==
                std::vector<uint8_t>(expected_owned.begin(), expected_owned.end()),
            "owned TX did not preserve copied bytes");
    wait_for(cv, mutex, [&] { return owned_done; }, "owned TX callback did not complete");
    require(!owned_error, "owned TX reported an error");
}

void test_buffered_read_regression()
{
    PseudoTerminal terminal;
    auto serial = open_serial(terminal);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> received;
    serial.on_data([&](std::span<const uint8_t> bytes)
    {
        std::lock_guard lock(mutex);
        received.insert(received.end(), bytes.begin(), bytes.end());
        cv.notify_all();
    });

    static constexpr std::array<uint8_t, 5> payload{21, 22, 23, 24, 25};
    terminal.write_all(payload);
    wait_for(cv, mutex, [&] { return received.size() == payload.size(); },
             "buffered RX callback did not complete");
    require(received == std::vector<uint8_t>(payload.begin(), payload.end()),
            "buffered RX payload mismatch");
}

void test_state_queries()
{
    PseudoTerminal terminal;
    auto serial = open_serial(terminal);

    require(serial.is_open(), "freshly opened port should report is_open()");
    require(serial.state() == SerialState::Connected, "freshly opened port should be Connected");

    serial.close();
    require(!serial.is_open(), "closed port should not report is_open()");
    require(serial.state() == SerialState::Disconnected, "closed port should be Disconnected");
}

void test_runtime_reconfiguration()
{
    PseudoTerminal terminal;
    auto serial = open_serial(terminal); // 115200 8N1

    require(serial.set_baud_rate(57600).has_value(), "set_baud_rate failed");
    require(serial.set_parity(Parity::Odd).has_value(), "set_parity failed");
    require(serial.set_stop_bits(StopBits::Two).has_value(), "set_stop_bits failed");
    require(serial.set_data_bits(DataBits::Seven).has_value(), "set_data_bits failed");
    require(serial.is_open(), "runtime reconfiguration closed the port");

#if defined(__linux__)
    // Parity enable and character size cannot be checked through a PTY (the
    // driver forces 8N1); they are covered by the hardware harness.
    const termios attributes = read_slave_termios(terminal);
    require(cfgetospeed(&attributes) == B57600, "runtime baud rate was not applied");
    require((attributes.c_cflag & CSTOPB) != 0, "runtime two stop bits were not applied");
#endif
}

void test_flow_control_api()
{
    PseudoTerminal terminal;
    auto opened = Serial::builder()
                  .baud_rate(115200)
                  .flow_control(FlowControl::Hardware)
                  .auto_reconnect(false)
                  .open(terminal.slave_name());
    require(opened.has_value(), "open with hardware flow control failed");
    auto serial = std::move(opened.value());

    require(serial.set_flow_control(FlowControl::Software).has_value(),
            "set_flow_control(Software) failed");
    require(serial.set_flow_control(FlowControl::None).has_value(),
            "set_flow_control(None) failed");
    require(serial.is_open(), "flow control reconfiguration closed the port");
}

#if defined(__linux__)
void test_flow_control_flags_linux()
{
    PseudoTerminal terminal;
    auto opened = Serial::builder()
                  .baud_rate(115200)
                  .flow_control(FlowControl::Hardware)
                  .auto_reconnect(false)
                  .open(terminal.slave_name());
    require(opened.has_value(), "open with hardware flow control failed");
    auto serial = std::move(opened.value());

    termios attributes = read_slave_termios(terminal);
    require((attributes.c_cflag & CRTSCTS) != 0, "hardware flow control did not set CRTSCTS");

    require(serial.set_flow_control(FlowControl::Software).has_value(),
            "set_flow_control(Software) failed");
    attributes = read_slave_termios(terminal);
    require((attributes.c_iflag & (IXON | IXOFF)) == (IXON | IXOFF),
            "software flow control did not set IXON|IXOFF");
    require((attributes.c_cflag & CRTSCTS) == 0,
            "software flow control left CRTSCTS enabled");

    require(serial.set_flow_control(FlowControl::None).has_value(),
            "set_flow_control(None) failed");
    attributes = read_slave_termios(terminal);
    require((attributes.c_iflag & (IXON | IXOFF)) == 0,
            "flow control None left IXON|IXOFF enabled");
    require((attributes.c_cflag & CRTSCTS) == 0,
            "flow control None left CRTSCTS enabled");
}
#endif

void test_runtime_set_on_closed_port()
{
    PseudoTerminal terminal;
    auto serial = open_serial(terminal);
    serial.close();

    const auto result = serial.set_baud_rate(115200);
    require(!result.has_value(), "set_baud_rate on a closed port should fail");
    require(result.error() == make_error_code(SerialError::DeviceDisconnected),
            "closed-port set_baud_rate returned the wrong error");
}
}

int main()
{
    try
    {
        test_borrowed_read_pause_resume_and_cancel();
        test_borrowed_and_owned_writes();
        test_buffered_read_regression();
        test_state_queries();
        test_runtime_reconfiguration();
        test_flow_control_api();
#if defined(__linux__)
        test_flow_control_flags_linux();
#endif
        test_runtime_set_on_closed_port();
        std::cout << "Astrial virtual serial tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Astrial virtual serial tests failed: " << error.what() << '\n';
        return 1;
    }
}
