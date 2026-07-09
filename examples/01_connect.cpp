#include <astrial.hpp>
#include <iostream>
#include <thread>

int main()
{
    auto serial =
        Serial::builder()
       .buad_rate(115200)
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

    static constexpr std::array<uint8_t, 3> cmd{0xA5, 0x5A, 0x01};
    
    if (auto res = serial.write(cmd); !res)
    {
        throw std::runtime_error(res.error().message());
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    return 0;
}
