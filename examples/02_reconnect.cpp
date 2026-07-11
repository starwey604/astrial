#include <astrial.hpp>
#include <iostream>
#include <thread>
#include <array>

int main()
{
    auto serial =
        Serial::builder()
       .buad_rate(115200)
       .parity(Parity::None)
       .stop_bits(StopBits::One)
       .auto_reconnect(true, std::chrono::seconds(2))
       .open("/dev/ttyACM0")
       .or_else([](auto&& e)
        {
            throw std::runtime_error(e.message());
        }).value();

    std::cout << "Connected. Auto-reconnect enabled (2s interval).\n";

    serial.on_data([](std::span<const uint8_t> data)
        {
            std::cout << "Received: " << data.size() << " bytes.\n";
            for (auto& c : data) std::cout << c;
            std::cout << "\n";
        }
    );

    serial.on_disconnect([](const std::error_code& ec)
        {
            std::cout << "Disconnected: " << ec.message() << ". Will attempt reconnect...\n";
        }
    );

    serial.on_reconnect([]()
        {
            std::cout << "Reconnected successfully.\n";
        }
    );

    static constexpr std::array<uint8_t, 7> cmd{0x61, 0x73, 0x74, 0x72, 0x69, 0x61, 0x6C};

    for (auto i = 0; i < 30; ++i)
    {
        if (auto res = serial.write(cmd); !res)
        {
            std::cout << "Write failed: " << res.error().message() << "\n";
        }
        else
        {
            std::cout << "Written " << cmd.size() << " bytes.\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
