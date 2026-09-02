/* SPDX-License-Identifier: MIT */

#include <astrial/Usb.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::string_view CsvVersion = "astrial_usb_lifecycle_v1";

struct Options
{
    std::uint16_t vendor_id{0x2fe3};
    std::uint16_t product_id{0x574c};
    std::string serial_number;
    std::size_t cycles{20};
    std::size_t read_size{512};
    std::chrono::milliseconds arm_timeout{2'000};
    std::chrono::milliseconds reconnect_timeout{15'000};
    std::chrono::milliseconds reopen_delay{20};
    bool wait_reconnect{};
    bool help{};
};

struct Counters
{
    std::atomic<std::uint64_t> provider_calls{};
    std::atomic<std::uint64_t> callbacks{};
    std::atomic<std::uint64_t> successful_reads{};
    std::atomic<std::uint64_t> cancelled_reads{};
    std::atomic<std::uint64_t> disconnected_reads{};
    std::atomic<std::uint64_t> stalled_reads{};
    std::atomic<std::uint64_t> unexpected_errors{};
    std::atomic<int> first_unexpected_error{};
    std::atomic<int> first_unexpected_source{};
    std::atomic<std::uint64_t> invalid_buffers{};
    std::atomic<std::uint64_t> disconnect_callbacks{};
    std::atomic<std::uint64_t> reconnect_callbacks{};
};

struct CycleResult
{
    std::uint64_t open_us{};
    std::uint64_t stop_us{};
    std::uint64_t close_us{};
    UsbBulkStats stats{};
    std::uint64_t provider_calls{};
    std::uint64_t callbacks{};
    std::uint64_t successful_reads{};
    std::uint64_t cancelled_reads{};
    std::uint64_t disconnected_reads{};
    std::uint64_t stalled_reads{};
    std::uint64_t disconnect_callbacks{};
    std::uint64_t reconnect_callbacks{};
};

void print_usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " [--vid HEX] [--pid HEX] [--serial TEXT] [--cycles N]"
           " [--read-size N] [--arm-timeout-ms N]"
           " [--reconnect-timeout-ms N] [--reopen-delay-ms N]"
           " [--wait-reconnect]\n\n"
        << "Normal mode repeatedly opens, starts, stops, restarts, and closes a"
           " physical Bulk interface. --wait-reconnect additionally waits for"
           " one externally-triggered disconnect/re-enumeration per cycle.\n";
}

std::uint64_t parse_integer(std::string_view text, int base,
                            std::string_view option)
{
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X")))
        text.remove_prefix(2);
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, base);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid value for " + std::string(option));
    return value;
}

std::string_view take_value(int& index, int argc, char** argv,
                            std::string_view argument,
                            std::string_view option)
{
    if (argument == option)
    {
        if (++index >= argc)
            throw std::invalid_argument("missing value for " + std::string(option));
        return argv[index];
    }
    const std::string prefix = std::string(option) + '=';
    if (argument.starts_with(prefix)) return argument.substr(prefix.size());
    return {};
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            options.help = true;
            continue;
        }
        if (argument == "--wait-reconnect")
        {
            options.wait_reconnect = true;
            continue;
        }
        if (const auto value = take_value(index, argc, argv, argument, "--vid");
            !value.empty())
        {
            const auto parsed = parse_integer(value, 16, "--vid");
            if (parsed == 0 || parsed > UINT16_MAX)
                throw std::invalid_argument("--vid must be 1..ffff");
            options.vendor_id = static_cast<std::uint16_t>(parsed);
            continue;
        }
        if (const auto value = take_value(index, argc, argv, argument, "--pid");
            !value.empty())
        {
            const auto parsed = parse_integer(value, 16, "--pid");
            if (parsed == 0 || parsed > UINT16_MAX)
                throw std::invalid_argument("--pid must be 1..ffff");
            options.product_id = static_cast<std::uint16_t>(parsed);
            continue;
        }
        if (const auto value = take_value(index, argc, argv, argument, "--serial");
            !value.empty())
        {
            options.serial_number = value;
            continue;
        }
        const auto parse_size = [&](std::string_view option) -> std::size_t
        {
            const auto value = take_value(index, argc, argv, argument, option);
            if (value.empty()) return 0;
            const auto parsed = parse_integer(value, 10, option);
            if (parsed > std::numeric_limits<std::size_t>::max())
                throw std::invalid_argument(std::string(option) + " is too large");
            return static_cast<std::size_t>(parsed);
        };
        if (const auto value = parse_size("--cycles"); value != 0)
        {
            options.cycles = value;
            continue;
        }
        if (const auto value = parse_size("--read-size"); value != 0)
        {
            options.read_size = value;
            continue;
        }
        if (const auto value = parse_size("--arm-timeout-ms"); value != 0)
        {
            options.arm_timeout = std::chrono::milliseconds(value);
            continue;
        }
        if (const auto value = parse_size("--reconnect-timeout-ms"); value != 0)
        {
            options.reconnect_timeout = std::chrono::milliseconds(value);
            continue;
        }
        if (argument == "--reopen-delay-ms" ||
            argument.starts_with("--reopen-delay-ms="))
        {
            const auto value = take_value(index, argc, argv, argument,
                                          "--reopen-delay-ms");
            options.reopen_delay = std::chrono::milliseconds(
                parse_integer(value, 10, "--reopen-delay-ms"));
            continue;
        }
        throw std::invalid_argument("unknown option: " + std::string(argument));
    }
    if (!options.help && (options.cycles == 0 || options.cycles > 10'000 ||
                          options.read_size == 0 || options.read_size > 16U * 1024U * 1024U))
        throw std::invalid_argument("cycles must be 1..10000 and read size 1..16 MiB");
    return options;
}

template <typename Predicate>
bool wait_until(Clock::time_point deadline, Predicate predicate)
{
    while (Clock::now() < deadline)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

std::uint64_t elapsed_us(Clock::time_point begin)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin)
            .count());
}

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

CycleResult run_cycle(const Options& options, std::size_t cycle)
{
    UsbBulkConfig config;
    config.device.vendor_id = options.vendor_id;
    config.device.product_id = options.product_id;
    config.device.serial_number = options.serial_number;
    config.read_queue_depth = 1;
    config.auto_reconnect = options.wait_reconnect;
    config.reconnect_interval = 50ms;

    const auto open_begin = Clock::now();
    auto opened = UsbBulkDevice::open(config);
    if (!opened) throw std::system_error(opened.error(), "USB open");
    const std::uint64_t open_us = elapsed_us(open_begin);
    UsbBulkDevice device = std::move(opened.value());
    require(device.state() == UsbState::Connected, "device did not enter Connected");
    const auto endpoints = device.endpoint_info();
    require(endpoints.maximum_packet_size_in != 0 &&
                endpoints.maximum_packet_size_out != 0,
            "bulk endpoint packet size is zero");

    std::vector<std::uint8_t> storage(options.read_size);
    Counters counters;
    const std::uintptr_t token = static_cast<std::uintptr_t>(cycle + 1U);
    const auto provider = [&]() -> UsbBorrowedBuffer
    {
        counters.provider_calls.fetch_add(1, std::memory_order_relaxed);
        return {{storage.data(), storage.size()}, token};
    };
    const auto callback = [&](const std::error_code& error,
                              UsbBorrowedBuffer buffer, std::size_t length)
    {
        counters.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (buffer.token != token || buffer.bytes.data() != storage.data() ||
            buffer.bytes.size() != storage.size() || length > storage.size())
            counters.invalid_buffers.fetch_add(1, std::memory_order_relaxed);
        if (!error)
            counters.successful_reads.fetch_add(1, std::memory_order_relaxed);
        else if (error == make_error_code(UsbError::TransferCancelled))
            counters.cancelled_reads.fetch_add(1, std::memory_order_relaxed);
        else if (error == make_error_code(UsbError::DeviceDisconnected))
            counters.disconnected_reads.fetch_add(1, std::memory_order_relaxed);
        else if (options.wait_reconnect &&
                 error == make_error_code(UsbError::TransferStalled))
            counters.stalled_reads.fetch_add(1, std::memory_order_relaxed);
        else
        {
            counters.unexpected_errors.fetch_add(1, std::memory_order_relaxed);
            int expected{};
            if (counters.first_unexpected_error.compare_exchange_strong(
                    expected, error.value(), std::memory_order_relaxed))
                counters.first_unexpected_source.store(1,
                                                       std::memory_order_relaxed);
        }
    };
    device.on_disconnect([&](const std::error_code& error)
    {
        if (error != make_error_code(UsbError::DeviceDisconnected))
        {
            counters.unexpected_errors.fetch_add(1, std::memory_order_relaxed);
            int expected{};
            if (counters.first_unexpected_error.compare_exchange_strong(
                    expected, error.value(), std::memory_order_relaxed))
                counters.first_unexpected_source.store(2,
                                                       std::memory_order_relaxed);
        }
        counters.disconnect_callbacks.fetch_add(1, std::memory_order_relaxed);
    });
    device.on_reconnect([&]
    {
        counters.reconnect_callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    auto started = device.start_reads(provider, callback);
    if (!started) throw std::system_error(started.error(), "start_reads");
    auto duplicate = device.start_reads(provider, callback);
    require(!duplicate && duplicate.error() == make_error_code(UsbError::InterfaceBusy),
            "duplicate start_reads did not return InterfaceBusy");
    require(wait_until(Clock::now() + options.arm_timeout, [&]
                       { return device.stats().reads_submitted != 0; }),
            "initial read was not submitted");

    const auto stop_begin = Clock::now();
    auto stopped = device.stop_reads();
    if (!stopped) throw std::system_error(stopped.error(), "stop_reads");
    const std::uint64_t stop_us = elapsed_us(stop_begin);
    require(device.stop_reads().has_value(), "idempotent stop_reads failed");
    auto after_stop = device.stats();
    require(after_stop.reads_submitted == after_stop.reads_completed,
            "stop_reads returned before all read transfers completed");

    started = device.start_reads(provider, callback);
    if (!started) throw std::system_error(started.error(), "restart reads");
    require(wait_until(Clock::now() + options.arm_timeout, [&]
                       { return device.stats().reads_submitted > after_stop.reads_submitted; }),
            "read was not resubmitted after restart");

    if (options.wait_reconnect)
    {
        const auto before_disconnect = device.stats().reads_submitted;
        std::cout << CsvVersion << ",phase,cycle=" << cycle
                  << ",state=waiting_for_disconnect" << std::endl;
        require(wait_until(Clock::now() + options.reconnect_timeout, [&]
                           {
                               return counters.disconnect_callbacks.load(
                                          std::memory_order_relaxed) != 0;
                           }),
                "timed out waiting for USB disconnect");
        require(device.state() == UsbState::Reconnecting ||
                    device.state() == UsbState::Connected,
                "disconnect did not enter Reconnecting");
        require(wait_until(Clock::now() + options.reconnect_timeout, [&]
                           {
                               return counters.reconnect_callbacks.load(
                                          std::memory_order_relaxed) != 0 &&
                                      device.state() == UsbState::Connected;
                           }),
                "timed out waiting for USB reconnect");
        require(wait_until(Clock::now() + options.arm_timeout, [&]
                           {
                               return device.stats().reads_submitted >
                                      before_disconnect;
                           }),
                "read transfer was not rearmed after reconnect");
    }

    const auto close_begin = Clock::now();
    device.close();
    const std::uint64_t close_us = elapsed_us(close_begin);
    require(device.state() == UsbState::Closed, "close did not enter Closed");
    device.close();

    const auto stats = device.stats();
    require(stats.reads_submitted == stats.reads_completed,
            "close returned before all read transfers completed");
    require(counters.callbacks.load(std::memory_order_relaxed) ==
                stats.reads_completed,
            "a borrowed buffer was not returned exactly once");
    require(counters.invalid_buffers.load(std::memory_order_relaxed) == 0,
            "read callback returned an invalid borrowed buffer");
    if (counters.unexpected_errors.load(std::memory_order_relaxed) != 0)
        throw std::runtime_error(
            "unexpected USB completion error value=" +
            std::to_string(counters.first_unexpected_error.load(
                std::memory_order_relaxed)) +
            " source=" +
            (counters.first_unexpected_source.load(std::memory_order_relaxed) == 1
                 ? "read"
                 : "disconnect"));
    if (options.wait_reconnect)
    {
        require(counters.disconnect_callbacks.load(std::memory_order_relaxed) == 1,
                "disconnect callback count was not one");
        require(counters.reconnect_callbacks.load(std::memory_order_relaxed) == 1,
                "reconnect callback count was not one");
        require(stats.reconnects == 1, "USB reconnect counter was not one");
        require(counters.stalled_reads.load(std::memory_order_relaxed) <=
                    config.read_queue_depth,
                "more than one read slot stalled during USB re-enumeration");
    }

    auto closed_start = device.start_reads(provider, callback);
    require(!closed_start &&
                closed_start.error() == make_error_code(UsbError::DeviceDisconnected),
            "start_reads after close did not fail as disconnected");
    const std::array<std::uint8_t, 1> byte{};
    auto closed_write = device.async_write_borrowed(byte, {});
    require(!closed_write &&
                closed_write.error() == make_error_code(UsbError::DeviceDisconnected),
            "write after close did not fail as disconnected");

    return {
        .open_us = open_us,
        .stop_us = stop_us,
        .close_us = close_us,
        .stats = stats,
        .provider_calls = counters.provider_calls.load(std::memory_order_relaxed),
        .callbacks = counters.callbacks.load(std::memory_order_relaxed),
        .successful_reads = counters.successful_reads.load(std::memory_order_relaxed),
        .cancelled_reads = counters.cancelled_reads.load(std::memory_order_relaxed),
        .disconnected_reads = counters.disconnected_reads.load(std::memory_order_relaxed),
        .stalled_reads = counters.stalled_reads.load(std::memory_order_relaxed),
        .disconnect_callbacks =
            counters.disconnect_callbacks.load(std::memory_order_relaxed),
        .reconnect_callbacks =
            counters.reconnect_callbacks.load(std::memory_order_relaxed),
    };
}

std::uint64_t percentile(std::vector<std::uint64_t> values, double fraction)
{
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1U));
    return values[index];
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parse_options(argc, argv);
        if (options.help)
        {
            print_usage(argv[0]);
            return 0;
        }

        std::vector<CycleResult> results;
        results.reserve(options.cycles);
        for (std::size_t cycle = 0; cycle < options.cycles; ++cycle)
        {
            auto result = run_cycle(options, cycle);
            std::cout << CsvVersion << ",cycle,index=" << cycle
                      << ",open_us=" << result.open_us
                      << ",stop_us=" << result.stop_us
                      << ",close_us=" << result.close_us
                      << ",providers=" << result.provider_calls
                      << ",callbacks=" << result.callbacks
                      << ",read_ok=" << result.successful_reads
                      << ",read_cancelled=" << result.cancelled_reads
                      << ",read_disconnected=" << result.disconnected_reads
                      << ",read_stalled=" << result.stalled_reads
                      << ",disconnect_callbacks=" << result.disconnect_callbacks
                      << ",reconnect_callbacks=" << result.reconnect_callbacks
                      << ",libusb_errors=" << result.stats.errors
                      << std::endl;
            results.push_back(result);
            if (cycle + 1U != options.cycles)
                std::this_thread::sleep_for(options.reopen_delay);
        }

        std::vector<std::uint64_t> opens;
        std::vector<std::uint64_t> stops;
        std::vector<std::uint64_t> closes;
        for (const auto& result : results)
        {
            opens.push_back(result.open_us);
            stops.push_back(result.stop_us);
            closes.push_back(result.close_us);
        }
        std::cout << CsvVersion << ",summary,result=pass,cycles=" << results.size()
                  << ",reconnect_mode=" << (options.wait_reconnect ? 1 : 0)
                  << ",open_p50_us=" << percentile(opens, 0.50)
                  << ",open_p99_us=" << percentile(opens, 0.99)
                  << ",open_max_us=" << *std::max_element(opens.begin(), opens.end())
                  << ",stop_p99_us=" << percentile(stops, 0.99)
                  << ",stop_max_us=" << *std::max_element(stops.begin(), stops.end())
                  << ",close_p99_us=" << percentile(closes, 0.99)
                  << ",close_max_us=" << *std::max_element(closes.begin(), closes.end())
                  << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << CsvVersion << ",summary,result=fail,error="
                  << std::quoted(error.what()) << '\n';
        return 1;
    }
}
