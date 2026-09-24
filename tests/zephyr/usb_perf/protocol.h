/* SPDX-License-Identifier: MIT */

#ifndef ASTRIAL_USB_PERF_PROTOCOL_H
#define ASTRIAL_USB_PERF_PROTOCOL_H

/* A bulk request of at least 32 bytes with this prefix returns device-local
 * timestamps.
 * The rest of the packet retains the caller's bytes, except these fields:
 *   8..15: cycle counter when OUT completion is handled
 *  16..23: cycle counter just before IN is submitted
 *  24..27: cycle counter frequency in Hz
 *  28..31: protocol version (currently 1)
 * Host and device clocks are not synchronized; only their differences are used.
 */
#define ASTRIAL_USB_TIMING_MAGIC "ASTRTIME"
#define ASTRIAL_USB_TIMING_MAGIC_SIZE 8U
#define ASTRIAL_USB_TIMING_HEADER_SIZE 32U
#define ASTRIAL_USB_TIMING_VERSION 1U

#endif
