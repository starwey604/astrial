/* SPDX-License-Identifier: MIT */

#include <astrial.hpp>

#include <cassert>

int main()
{
    const auto devices = UsbBulkDevice::list_devices();
    assert(devices.has_value());

    UsbBulkConfig invalid;
    const auto invalid_result = UsbBulkDevice::open(invalid);
    assert(!invalid_result);
    assert(invalid_result.error() == make_error_code(UsbError::InvalidArgument));

    UsbBulkConfig missing;
    missing.vendor_id = 0xffff;
    missing.product_id = 0xffff;
    missing.auto_reconnect = false;
    const auto missing_result = UsbBulkDevice::open(missing);
    assert(!missing_result);
    assert(missing_result.error() == make_error_code(UsbError::DeviceNotFound));

    assert(make_error_code(UsbError::TransferStalled).message() ==
           "USB endpoint stalled");
}
