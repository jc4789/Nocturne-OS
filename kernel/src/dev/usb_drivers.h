/* USB の class の実装（dev/usb_drivers.c の表）。番号は struct usb_device の state[] の場所でもある。hub はバスの
   一部で、表に入らない。 */
#pragma once

enum { USB_DRIVER_HID, USB_DRIVER_MSC, USB_DRIVER_NET, USB_DRIVERS };

struct usb_driver;
extern const struct usb_driver usbhid_driver, usbmsc_driver, usbnet_driver;
