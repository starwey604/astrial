/* SPDX-License-Identifier: ISC */

#include <astrial.hpp>

#include <iostream>
#include <set>
#include <string>

int main()
{
    const auto ports = Serial::list_ports();
    std::set<std::string> names;
    for (const auto& port : ports)
    {
        if (port.port_name.empty())
        {
            std::cerr << "Serial::list_ports() returned an empty port name\n";
            return 1;
        }
        if (!names.insert(port.port_name).second)
        {
            std::cerr << "Serial::list_ports() returned a duplicate port: "
                      << port.port_name << '\n';
            return 1;
        }
    }
    std::cout << "Serial::list_ports() returned " << ports.size() << " port(s)\n";
    return 0;
}
