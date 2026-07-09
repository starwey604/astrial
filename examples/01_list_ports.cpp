#include <astrial.hpp>
#include <iostream>

int main()
{
    auto ports = Serial::list_ports();
    std::cout << "listing all port available:" << "\n";
    for (auto& port : ports)
    {
        std::cout << port << "\n";
    }
    return 0;
}
