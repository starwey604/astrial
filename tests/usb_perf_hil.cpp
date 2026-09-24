/* SPDX-License-Identifier: MIT */

#include <astrial/Usb.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "zephyr/usb_perf/protocol.h"

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Options
{
    std::uint16_t vid{0x2fe3};
    std::uint16_t pid{0x574d};
    bool profile{};
    bool zlp{};
#ifdef ASTRIAL_USB_LATENCY_TEST
    std::size_t size{64};
    std::size_t rounds{300};
    std::size_t warmup{20};
#else
    std::size_t size{64 * 1024};
    std::size_t rounds{64};
    std::size_t warmup{4};
#endif
};

std::size_t parse_number(std::string_view value, int base)
{
    if (base == 16 && value.starts_with("0x")) value.remove_prefix(2);
    std::size_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result, base);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid number: " + std::string(value));
    return result;
}

Options parse_options(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help")
        {
            std::cout << "Usage: " << argv[0]
                      << " [--size BYTES] [--rounds N] [--warmup N] [--profile] [--zlp]"
                         " [--vid HEX] [--pid HEX]\n";
            std::exit(0);
        }
        if (arg == "--profile")
        {
            options.profile = true;
            continue;
        }
        if (arg == "--zlp")
        {
            options.zlp = true;
            continue;
        }
        if (++i >= argc) throw std::invalid_argument("missing value for " + std::string(arg));
        const std::string_view value = argv[i];
        if (arg == "--size") options.size = parse_number(value, 10);
        else if (arg == "--rounds") options.rounds = parse_number(value, 10);
        else if (arg == "--warmup") options.warmup = parse_number(value, 10);
        else if (arg == "--vid" || arg == "--pid")
        {
            const auto parsed = parse_number(value, 16);
            if (parsed == 0 || parsed > UINT16_MAX)
                throw std::invalid_argument("VID/PID must be 1..ffff");
            if (arg == "--vid") options.vid = static_cast<std::uint16_t>(parsed);
            else options.pid = static_cast<std::uint16_t>(parsed);
        }
        else throw std::invalid_argument("unknown option: " + std::string(arg));
    }
    if (options.size == 0 || options.size > 1024 * 1024 ||
        options.rounds == 0 || options.rounds > 100000 ||
        options.warmup > 10000 || options.vid == 0 || options.pid == 0)
        throw std::invalid_argument("size must be 1..1048576; rounds 1..100000");
    if (options.profile && options.size < ASTRIAL_USB_TIMING_HEADER_SIZE)
        throw std::invalid_argument("--profile requires at least 32 bytes");
    return options;
}

std::uint64_t get_le64(const std::uint8_t* bytes)
{
    std::uint64_t value{};
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

std::uint32_t get_le32(const std::uint8_t* bytes)
{
    std::uint32_t value{};
    for (int i = 3; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

struct Sample
{
    double rtt_us{};
    double device_us{};
    double write_done_us{};
};

class EchoSession
{
public:
    EchoSession(UsbBulkDevice&& opened, std::size_t size, bool profile, bool zlp)
        : device_(std::move(opened)), payload_(size), profile_(profile), zlp_(zlp)
    {
        const auto info = device_.endpoint_info();
        if (info.maximum_packet_size_in != 512 ||
            info.maximum_packet_size_out != 512)
            throw std::runtime_error("expected high-speed 512-byte bulk endpoints");
        auto started = device_.start_reads(
            {.buffer_size = std::min(size, std::size_t{64 * 1024})},
            [this](const std::error_code& error, std::span<const std::uint8_t> data)
            {
                const auto callback_at = Clock::now();
                std::lock_guard lock(mutex_);
                if (error)
                {
                    error_ = "read: " + error.message();
                }
                else if (received_ + data.size() > payload_.size())
                {
                    error_ = "received too many bytes";
                }
                else
                {
                    for (std::size_t i = 0; i < data.size(); ++i)
                    {
                        const auto offset = received_ + i;
                        if (profile_ && offset >= 8 &&
                            offset < ASTRIAL_USB_TIMING_HEADER_SIZE)
                            continue;
                        if (data[i] != payload_[offset])
                        {
                            error_ = "echo data mismatch at byte " +
                                     std::to_string(offset);
                            break;
                        }
                    }
                    if (profile_ && received_ == 0 &&
                        data.size() >= ASTRIAL_USB_TIMING_HEADER_SIZE)
                    {
                        const auto out = get_le64(data.data() + 8);
                        const auto in = get_le64(data.data() + 16);
                        const auto hz = get_le32(data.data() + 24);
                        const auto version = get_le32(data.data() + 28);
                        if (version != ASTRIAL_USB_TIMING_VERSION || hz == 0 || in < out)
                            error_ = "invalid firmware timestamps";
                        else
                            device_us_ = static_cast<double>(in - out) * 1e6 / hz;
                    }
                    received_ += data.size();
                    if (received_ == payload_.size())
                        echoed_at_ = callback_at;
                }
                cv_.notify_all();
            });
        if (!started) throw std::system_error(started.error(), "start_reads");
    }

    ~EchoSession()
    {
        (void)device_.stop_reads();
        device_.close();
    }

    Sample exchange(std::uint32_t sequence)
    {
        {
            std::lock_guard lock(mutex_);
            for (std::size_t i = 0; i < payload_.size(); ++i)
                payload_[i] = static_cast<std::uint8_t>((i * 131u + sequence * 17u) & 0xffu);
            if (profile_)
                std::copy_n(ASTRIAL_USB_TIMING_MAGIC,
                            ASTRIAL_USB_TIMING_MAGIC_SIZE, payload_.begin());
            received_ = 0;
            write_done_ = false;
            error_.clear();
            echoed_at_ = {};
            write_completed_at_ = {};
            device_us_ = 0;
        }

        const auto began = Clock::now();
        auto submitted = device_.async_write_borrowed(payload_,
            [this](const std::error_code& error, std::size_t written)
            {
                const auto callback_at = Clock::now();
                std::lock_guard lock(mutex_);
                if (error) error_ = "write: " + error.message();
                else if (written != payload_.size()) error_ = "short write";
                write_done_ = true;
                write_completed_at_ = callback_at;
                cv_.notify_all();
            }, {.append_zero_length_packet = zlp_});
        if (!submitted) throw std::system_error(submitted.error(), "async_write_borrowed");

        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, 30s, [this]
            { return !error_.empty() || (write_done_ && echoed_at_ != Clock::time_point{}); }))
            throw std::runtime_error("echo timeout after " +
                                     std::to_string(received_) + " bytes");
        if (!error_.empty()) throw std::runtime_error(error_);
        return {
            .rtt_us = std::chrono::duration<double, std::micro>(echoed_at_ - began).count(),
            .device_us = device_us_,
            .write_done_us = std::chrono::duration<double, std::micro>(
                write_completed_at_ - began).count(),
        };
    }

private:
    UsbBulkDevice device_;
    std::vector<std::uint8_t> payload_;
    bool profile_{};
    bool zlp_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t received_{};
    bool write_done_{};
    Clock::time_point echoed_at_{};
    Clock::time_point write_completed_at_{};
    double device_us_{};
    std::string error_;
};

double percentile(const std::vector<double>& sorted, double fraction)
{
    const auto index = static_cast<std::size_t>((sorted.size() - 1) * fraction);
    return sorted[index];
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const auto options = parse_options(argc, argv);
        UsbBulkConfig config;
        config.device.vendor_id = options.vid;
        config.device.product_id = options.pid;
        config.read_queue_depth = 1;
        config.auto_reconnect = false;
        auto opened = UsbBulkDevice::open(config);
        if (!opened) throw std::system_error(opened.error(), "USB open");
        EchoSession session(std::move(opened.value()), options.size,
                            options.profile, options.zlp);

        for (std::size_t i = 0; i < options.warmup; ++i)
            (void)session.exchange(static_cast<std::uint32_t>(i));
        std::vector<double> rtts;
        std::vector<double> device_times;
        std::vector<double> write_times;
        rtts.reserve(options.rounds);
        device_times.reserve(options.rounds);
        write_times.reserve(options.rounds);
        const auto start = Clock::now();
        for (std::size_t i = 0; i < options.rounds; ++i)
        {
            const auto sample = session.exchange(static_cast<std::uint32_t>(i + options.warmup));
            rtts.push_back(sample.rtt_us);
            device_times.push_back(sample.device_us);
            write_times.push_back(sample.write_done_us);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::sort(rtts.begin(), rtts.end());
        std::sort(device_times.begin(), device_times.end());
        std::sort(write_times.begin(), write_times.end());
        const double mib = static_cast<double>(options.size) * options.rounds /
                           (1024.0 * 1024.0);
        std::cout << std::fixed << std::setprecision(2)
                  << "size=" << options.size << " rounds=" << options.rounds
                  << " zlp=" << (options.zlp ? 1 : 0)
                  << " verified_bytes=" << options.size * options.rounds
                  << " seconds=" << seconds
                  << " echo_MiB_per_s=" << mib / seconds
                  << " rtt_us_min=" << rtts.front()
                  << " p50=" << percentile(rtts, 0.50)
                  << " p95=" << percentile(rtts, 0.95)
                  << " p99=" << percentile(rtts, 0.99)
                  << " max=" << rtts.back();
        if (options.profile)
            std::cout << " device_us_p50=" << percentile(device_times, 0.50)
                      << " device_us_p99=" << percentile(device_times, 0.99)
                      << " write_done_us_p50=" << percentile(write_times, 0.50);
        std::cout << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "USB HIL failed: " << error.what() << '\n';
        return 1;
    }
}
