#include <astrial.hpp>
#include <iostream>
#include <thread>
#include <array>

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

    serial.on_data([](std::span<const uint8_t> data)
        {
            std::cout << "Received: " << data.size() << " bytes.\n";
            for (auto& c : data)
            {
                std::cout << c;
            }
            std::cout << "\n";
        }
    );

    static constexpr std::array<uint8_t, 7> cmd{0x61, 0x73, 0x74, 0x72, 0x69, 0x61, 0x6C};

    for (auto i = 0; i < 3; ++i)
    {
        if (auto res = serial.write(cmd); !res)
        {
            throw std::runtime_error(res.error().message());
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }


    return 0;
}
