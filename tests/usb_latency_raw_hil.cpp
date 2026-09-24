/* SPDX-License-Identifier: MIT */

// Same bulk timing request as usb_perf_hil, using libusb directly. This is a
// diagnostic baseline for the Astrial path, not an application API example.
#include <libusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "zephyr/usb_perf/protocol.h"

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::size_t number(std::string_view text)
{
    std::size_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid number: " + std::string(text));
    return result;
}

std::uint64_t le64(const unsigned char* bytes)
{
    std::uint64_t value{};
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

std::uint32_t le32(const unsigned char* bytes)
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

class RawSession
{
public:
    explicit RawSession(std::size_t size) : tx_(size), rx_(size)
    {
        check(libusb_init(&context_), "libusb_init");
        handle_ = libusb_open_device_with_vid_pid(context_, 0x2fe3, 0x574d);
        if (handle_ == nullptr) throw std::runtime_error("open 2fe3:574d failed");
        check(libusb_claim_interface(handle_, 0), "claim interface 0");
        claimed_ = true;
        in_ = libusb_alloc_transfer(0);
        out_ = libusb_alloc_transfer(0);
        if (in_ == nullptr || out_ == nullptr)
            throw std::runtime_error("libusb_alloc_transfer failed");
        events_ = std::thread([this]
        {
            while (!stopping_ || in_flight_ || out_flight_)
            {
                timeval timeout{0, 10000};
                const int rc = libusb_handle_events_timeout_completed(context_,
                                                                      &timeout, nullptr);
                if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_INTERRUPTED)
                {
                    std::lock_guard lock(mutex_);
                    error_ = "event handler: " + std::string(libusb_error_name(rc));
                    cv_.notify_all();
                }
            }
        });
    }

    ~RawSession()
    {
        stopping_ = true;
        if (in_flight_) (void)libusb_cancel_transfer(in_);
        if (out_flight_) (void)libusb_cancel_transfer(out_);
        if (context_ != nullptr) libusb_interrupt_event_handler(context_);
        if (events_.joinable()) events_.join();
        if (in_ != nullptr) libusb_free_transfer(in_);
        if (out_ != nullptr) libusb_free_transfer(out_);
        if (claimed_) libusb_release_interface(handle_, 0);
        if (handle_ != nullptr) libusb_close(handle_);
        if (context_ != nullptr) libusb_exit(context_);
    }

    Sample exchange(std::uint32_t sequence)
    {
        {
            std::lock_guard lock(mutex_);
            error_.clear();
            in_done_ = false;
            out_done_ = false;
            for (std::size_t i = 0; i < tx_.size(); ++i)
                tx_[i] = static_cast<unsigned char>((i * 131u + sequence * 17u) & 0xffu);
            std::copy_n(ASTRIAL_USB_TIMING_MAGIC,
                        ASTRIAL_USB_TIMING_MAGIC_SIZE, tx_.begin());
        }
        libusb_fill_bulk_transfer(in_, handle_, 0x81, rx_.data(),
                                  static_cast<int>(rx_.size()), on_in, this, 0);
        in_flight_ = true;
        const int in_rc = libusb_submit_transfer(in_);
        if (in_rc != LIBUSB_SUCCESS) in_flight_ = false;
        check(in_rc, "submit IN");
        libusb_fill_bulk_transfer(out_, handle_, 0x01, tx_.data(),
                                  static_cast<int>(tx_.size()), on_out, this, 0);
        began_ = Clock::now();
        out_flight_ = true;
        const int out_rc = libusb_submit_transfer(out_);
        if (out_rc != LIBUSB_SUCCESS) out_flight_ = false;
        check(out_rc, "submit OUT");

        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, 30s, [this]
            { return !error_.empty() || (in_done_ && out_done_); }))
            throw std::runtime_error("raw echo timeout");
        if (!error_.empty()) throw std::runtime_error(error_);
        return {
            .rtt_us = std::chrono::duration<double, std::micro>(read_at_ - began_).count(),
            .device_us = device_us_,
            .write_done_us = std::chrono::duration<double, std::micro>(wrote_at_ - began_).count(),
        };
    }

private:
    static void check(int rc, const char* operation)
    {
        if (rc != LIBUSB_SUCCESS)
            throw std::runtime_error(std::string(operation) + ": " + libusb_error_name(rc));
    }

    static void LIBUSB_CALL on_in(libusb_transfer* transfer)
    {
        auto& self = *static_cast<RawSession*>(transfer->user_data);
        const auto callback_at = Clock::now();
        std::lock_guard lock(self.mutex_);
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED ||
            transfer->actual_length != static_cast<int>(self.rx_.size()))
        {
            self.error_ = "IN transfer incomplete";
        }
        else
        {
            for (std::size_t i = 0; i < self.rx_.size(); ++i)
            {
                if (i >= 8 && i < ASTRIAL_USB_TIMING_HEADER_SIZE) continue;
                if (self.rx_[i] != self.tx_[i])
                {
                    self.error_ = "echo mismatch at byte " + std::to_string(i);
                    break;
                }
            }
            const auto out = le64(self.rx_.data() + 8);
            const auto in = le64(self.rx_.data() + 16);
            const auto hz = le32(self.rx_.data() + 24);
            if (le32(self.rx_.data() + 28) != ASTRIAL_USB_TIMING_VERSION ||
                hz == 0 || in < out)
                self.error_ = "invalid firmware timestamps";
            else
                self.device_us_ = static_cast<double>(in - out) * 1e6 / hz;
        }
        self.read_at_ = callback_at;
        self.in_done_ = true;
        self.in_flight_ = false;
        self.cv_.notify_all();
    }

    static void LIBUSB_CALL on_out(libusb_transfer* transfer)
    {
        auto& self = *static_cast<RawSession*>(transfer->user_data);
        const auto callback_at = Clock::now();
        std::lock_guard lock(self.mutex_);
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED ||
            transfer->actual_length != static_cast<int>(self.tx_.size()))
            self.error_ = "OUT transfer incomplete";
        self.wrote_at_ = callback_at;
        self.out_done_ = true;
        self.out_flight_ = false;
        self.cv_.notify_all();
    }

    libusb_context* context_{};
    libusb_device_handle* handle_{};
    libusb_transfer* in_{};
    libusb_transfer* out_{};
    bool claimed_{};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> in_flight_{false};
    std::atomic<bool> out_flight_{false};
    std::thread events_;
    std::vector<unsigned char> tx_;
    std::vector<unsigned char> rx_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool in_done_{};
    bool out_done_{};
    std::string error_;
    Clock::time_point began_{};
    Clock::time_point read_at_{};
    Clock::time_point wrote_at_{};
    double device_us_{};
};

double percentile(std::vector<double>& data, double fraction)
{
    std::sort(data.begin(), data.end());
    return data[static_cast<std::size_t>((data.size() - 1) * fraction)];
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::size_t size = 64;
        std::size_t rounds = 1000;
        std::size_t warmup = 50;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--help")
            {
                std::cout << "Usage: " << argv[0]
                          << " [--size BYTES] [--rounds N] [--warmup N]\n";
                return 0;
            }
            if (++i >= argc) throw std::invalid_argument("missing value");
            if (arg == "--size") size = number(argv[i]);
            else if (arg == "--rounds") rounds = number(argv[i]);
            else if (arg == "--warmup") warmup = number(argv[i]);
            else throw std::invalid_argument("unknown option: " + std::string(arg));
        }
        if (size < 32 || size > 511 || rounds == 0 || rounds > 100000 ||
            warmup > 10000)
            throw std::invalid_argument("size must be 32..511; rounds 1..100000");
        RawSession session(size);
        for (std::size_t i = 0; i < warmup; ++i)
            (void)session.exchange(static_cast<std::uint32_t>(i));
        std::vector<double> rtt, device, write;
        rtt.reserve(rounds);
        device.reserve(rounds);
        write.reserve(rounds);
        for (std::size_t i = 0; i < rounds; ++i)
        {
            const auto sample = session.exchange(static_cast<std::uint32_t>(i + warmup));
            rtt.push_back(sample.rtt_us);
            device.push_back(sample.device_us);
            write.push_back(sample.write_done_us);
        }
        std::cout << std::fixed << std::setprecision(2)
                  << "raw_libusb size=" << size << " rounds=" << rounds
                  << " rtt_us_p50=" << percentile(rtt, 0.50)
                  << " rtt_us_p99=" << percentile(rtt, 0.99)
                  << " device_us_p50=" << percentile(device, 0.50)
                  << " write_done_us_p50=" << percentile(write, 0.50) << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "raw USB HIL failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
