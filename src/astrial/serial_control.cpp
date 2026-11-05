#include "serial_control.hpp"

#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#ifdef __linux__
#include <linux/serial.h>
#endif
#endif

namespace detail
{
namespace
{
std::error_code native_error()
{
#ifdef _WIN32
    const auto error = ::GetLastError();
    if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION)
        return make_error_code(SerialError::UnsupportedOperation);
    return {static_cast<int>(error), std::system_category()};
#else
    const int error = errno;
    if (error == ENOTTY || error == ENOSYS || error == EOPNOTSUPP)
        return make_error_code(SerialError::UnsupportedOperation);
    return {error, std::generic_category()};
#endif
}
}

tl::expected<void, std::error_code> set_modem_output(asio::serial_port& port,
                                                     bool dtr, bool asserted)
{
#ifdef _WIN32
    DCB state{};
    state.DCBlength = sizeof(state);
    if (!::GetCommState(port.native_handle(), &state))
        return tl::make_unexpected(native_error());
    if (dtr)
        state.fDtrControl = asserted ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    else
    {
        if (state.fRtsControl == RTS_CONTROL_HANDSHAKE)
            return tl::make_unexpected(make_error_code(SerialError::UnsupportedOperation));
        state.fRtsControl = asserted ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    }
    if (!::SetCommState(port.native_handle(), &state))
        return tl::make_unexpected(native_error());
#else
    // Change only the requested output; do not write back a stale TIOCMGET
    // snapshot and accidentally overwrite another line managed by the driver.
    int bit = dtr ? TIOCM_DTR : TIOCM_RTS;
    if (::ioctl(port.native_handle(), asserted ? TIOCMBIS : TIOCMBIC, &bit) < 0)
        return tl::make_unexpected(native_error());
#endif
    return {};
}

tl::expected<PinoutSignals, std::error_code> read_pinout(asio::serial_port& port)
{
    PinoutSignals result = PinoutSignals::None;
#ifdef _WIN32
    DWORD input{};
    if (!::GetCommModemStatus(port.native_handle(), &input))
        return tl::make_unexpected(native_error());
    if (input & MS_CTS_ON) result |= PinoutSignals::ClearToSend;
    if (input & MS_DSR_ON) result |= PinoutSignals::DataSetReady;
    if (input & MS_RLSD_ON) result |= PinoutSignals::DataCarrierDetect;
    if (input & MS_RING_ON) result |= PinoutSignals::RingIndicator;

    // IOCTL_SERIAL_GET_DTRRTS (documented by Microsoft). DCB configuration is
    // not the current output state, particularly under hardware handshaking.
    constexpr DWORD get_dtr_rts = (0x1bUL << 16) | (30UL << 2);
    DWORD outputs{}, bytes{};
    OVERLAPPED operation{};
    const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return tl::make_unexpected(native_error());
    // ASIO owns this handle's IOCP. Suppress a completion packet for our
    // stack-owned OVERLAPPED so ASIO cannot mistake it for its own operation.
    operation.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(event) | 1);
    BOOL success = ::DeviceIoControl(port.native_handle(), get_dtr_rts, nullptr, 0,
                                    &outputs, sizeof(outputs), &bytes, &operation);
    if (!success && ::GetLastError() == ERROR_IO_PENDING)
        success = ::GetOverlappedResult(port.native_handle(), &operation, &bytes, TRUE);
    const auto error = success ? std::error_code{} : native_error();
    ::CloseHandle(event);
    if (error) return tl::make_unexpected(error);
    if (outputs & 0x01) result |= PinoutSignals::DataTerminalReady;
    if (outputs & 0x02) result |= PinoutSignals::RequestToSend;
#else
    int bits{};
    if (::ioctl(port.native_handle(), TIOCMGET, &bits) < 0)
        return tl::make_unexpected(native_error());
    if (bits & TIOCM_DTR) result |= PinoutSignals::DataTerminalReady;
    if (bits & TIOCM_RTS) result |= PinoutSignals::RequestToSend;
    if (bits & TIOCM_CTS) result |= PinoutSignals::ClearToSend;
    if (bits & TIOCM_DSR) result |= PinoutSignals::DataSetReady;
    if (bits & TIOCM_CAR) result |= PinoutSignals::DataCarrierDetect;
    if (bits & TIOCM_RNG) result |= PinoutSignals::RingIndicator;
#endif
    return result;
}

tl::expected<void, std::error_code> set_break(asio::serial_port& port, bool enabled)
{
#ifdef _WIN32
    const BOOL success = enabled ? ::SetCommBreak(port.native_handle())
                                 : ::ClearCommBreak(port.native_handle());
    if (!success) return tl::make_unexpected(native_error());
#else
    if (::ioctl(port.native_handle(), enabled ? TIOCSBRK : TIOCCBRK) < 0)
        return tl::make_unexpected(native_error());
#endif
    return {};
}

tl::expected<SerialLineErrorCounters, std::error_code> read_line_errors(asio::serial_port& port)
{
#ifdef __linux__
    serial_icounter_struct counters{};
    if (::ioctl(port.native_handle(), TIOCGICOUNT, &counters) < 0)
        return tl::make_unexpected(native_error());
    return SerialLineErrorCounters{
        static_cast<uint32_t>(counters.parity), static_cast<uint32_t>(counters.frame),
        static_cast<uint32_t>(counters.overrun), static_cast<uint32_t>(counters.brk),
        static_cast<uint32_t>(counters.buf_overrun)};
#else
    // ClearCommError exposes and clears flags, not cumulative counters. Do not
    // manufacture Windows counts or claim a zero count on unsupported systems.
    (void)port;
    return tl::make_unexpected(make_error_code(SerialError::UnsupportedOperation));
#endif
}

tl::expected<void, std::error_code> set_native_parity(asio::serial_port& port, Parity parity)
{
#ifdef _WIN32
    DCB state{};
    state.DCBlength = sizeof(state);
    if (!::GetCommState(port.native_handle(), &state))
        return tl::make_unexpected(native_error());
    switch (parity)
    {
    case Parity::None: state.Parity = NOPARITY; break;
    case Parity::Odd: state.Parity = ODDPARITY; break;
    case Parity::Even: state.Parity = EVENPARITY; break;
    case Parity::Mark: state.Parity = MARKPARITY; break;
    case Parity::Space: state.Parity = SPACEPARITY; break;
    default: return tl::make_unexpected(make_error_code(SerialError::InvalidArgument));
    }
    state.fParity = parity != Parity::None;
    if (!::SetCommState(port.native_handle(), &state))
        return tl::make_unexpected(native_error());
#else
#ifndef CMSPAR
    if (parity == Parity::Mark || parity == Parity::Space)
        return tl::make_unexpected(make_error_code(SerialError::UnsupportedOperation));
#endif
    termios state{};
    if (::tcgetattr(port.native_handle(), &state) < 0)
        return tl::make_unexpected(native_error());
    state.c_cflag &= ~(PARENB | PARODD);
#ifdef CMSPAR
    state.c_cflag &= ~CMSPAR;
#endif
    switch (parity)
    {
    case Parity::None: state.c_iflag |= IGNPAR; break;
    case Parity::Odd: state.c_cflag |= PARENB | PARODD; break;
    case Parity::Even: state.c_cflag |= PARENB; break;
#ifdef CMSPAR
    case Parity::Mark: state.c_cflag |= PARENB | PARODD | CMSPAR; break;
    case Parity::Space: state.c_cflag |= PARENB | CMSPAR; break;
#endif
    default: return tl::make_unexpected(make_error_code(SerialError::InvalidArgument));
    }
    if (parity != Parity::None)
    {
        state.c_iflag &= ~(IGNPAR | PARMRK);
        state.c_iflag |= INPCK;
    }
    if (::tcsetattr(port.native_handle(), TCSANOW, &state) < 0)
        return tl::make_unexpected(native_error());
#endif
    return {};
}
}
