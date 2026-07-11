#ifndef ASTRIAL_PORT_HPP
#define ASTRIAL_PORT_HPP
#include "Types.hpp"
#include "tl/expected.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <filesystem>
#include <fstream>
#elif defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#pragma comment(lib, "setupapi.lib")
#endif


#endif //ASTRIAL_PORT_HPP
