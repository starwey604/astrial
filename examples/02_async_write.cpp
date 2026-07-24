#include <astrial.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <array>
#include <mutex>
#include <condition_variable>

int main()
{
    auto serial =
        Serial::builder()
       .baud_rate(115200)
       .parity(Parity::None)
       .stop_bits(StopBits::One)
       .open("/dev/ttyACM0")
       .or_else([](auto&& e)
        {
            throw std::runtime_error(e.message());
        }).value();

    static constexpr std::array<uint8_t, 7> cmd{0x61, 0x73, 0x74, 0x72, 0x69, 0x61, 0x6C};

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    auto send_time = std::chrono::steady_clock::now();

    serial.async_write(cmd, [&](const std::error_code& ec, std::size_t written)
        {
            auto complete_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                complete_time - send_time).count();

            std::lock_guard<std::mutex> lock(mtx);
            if (ec)
            {
                std::cout << "Async write failed: " << ec.message()
                          << " after " << elapsed << " us.\n";
            }
            else
            {
                std::cout << "Sent " << written << " bytes at " << send_time.time_since_epoch().count()
                          << ", completed at " << complete_time.time_since_epoch().count()
                          << " (" << elapsed << " us elapsed).\n";
            }
            done = true;
            cv.notify_one();
        });

    std::cout << "Async write issued at " << send_time.time_since_epoch().count() << "\n";

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return done; });
    }

    return 0;
}
