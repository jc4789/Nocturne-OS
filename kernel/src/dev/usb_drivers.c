/* USB の class の実装の表。実装を足すのは、dev/usb_drivers.h に番号を足し、ここに一行足すことである。 */
#include "dev/usb.h"

const struct usb_driver *const usb_drivers[USB_DRIVERS] = {
    [USB_DRIVER_HID] = &usbhid_driver,
    [USB_DRIVER_MSC] = &usbmsc_driver,
    [USB_DRIVER_NET] = &usbnet_driver,
};
