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

    UsbBulkConfig invalid_endpoints;
    invalid_endpoints.device.vendor_id = 0xffff;
    invalid_endpoints.device.product_id = 0xffff;
    invalid_endpoints.bulk_interface.endpoint_in = 0x01;
    invalid_endpoints.bulk_interface.endpoint_out = 0x81;
    const auto invalid_endpoints_result = UsbBulkDevice::open(invalid_endpoints);
    assert(!invalid_endpoints_result);
    assert(invalid_endpoints_result.error() ==
           make_error_code(UsbError::InvalidArgument));

    UsbBulkConfig missing;
    missing.device.vendor_id = 0xffff;
    missing.device.product_id = 0xffff;
    missing.auto_reconnect = false;
    const auto missing_result = UsbBulkDevice::open(missing);
    assert(!missing_result);
    assert(missing_result.error() == make_error_code(UsbError::DeviceNotFound));

    assert(make_error_code(UsbError::TransferStalled).message() ==
           "USB endpoint stalled");
}
