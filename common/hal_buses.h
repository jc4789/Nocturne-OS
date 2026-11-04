/* 知られたバスの名前と性質の鍵（hal_abi.h）。バスを足すのは、ここに名前を足し、装置を hal_publish することで
   ある。HAL のコードは変わらない。カーネルの実装の規則と、サービスの宣言が同じ形を使う。 */
#pragma once
#include "hal_abi.h"

#define N_BUS_ROOT N_HAL_KEY('r', 'o', 'o', 0)

/* PCI の機能。class は 24 bit の class code（class、subclass、prog-if）。 */
#define N_BUS_PCI N_HAL_KEY('p', 'c', 'i', 0)
#define N_PROP_PCI_VENDOR N_HAL_KEY('p', 'c', 'i', 1)
#define N_PROP_PCI_DEVICE N_HAL_KEY('p', 'c', 'i', 2)
#define N_PROP_PCI_CLASS N_HAL_KEY('p', 'c', 'i', 3)
#define N_PROP_PCI_REVISION N_HAL_KEY('p', 'c', 'i', 4)
#define N_PROP_PCI_SUBSYSTEM_VENDOR N_HAL_KEY('p', 'c', 'i', 5)
#define N_PROP_PCI_SUBSYSTEM N_HAL_KEY('p', 'c', 'i', 6)
#define N_PROP_PCI_ADDRESS N_HAL_KEY('p', 'c', 'i', 7) /* N_PCI_TARGET */
#define N_PCI_THE_BUS N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_PCI)
#define N_PCI_CLASS_CODE(c, s, p) ((uint64_t)(c) << 16 | (uint64_t)(s) << 8 | (uint64_t)(p))
#define N_PCI_ID(v, d) N_HAL_RULE(N_PCI_THE_BUS, N_HAL_NUMBER(N_PROP_PCI_VENDOR, v), N_HAL_NUMBER(N_PROP_PCI_DEVICE, d))
#define N_PCI_CLASS(c, s) N_HAL_RULE(N_PCI_THE_BUS, N_HAL_MASKED(N_PROP_PCI_CLASS, N_PCI_CLASS_CODE(c, s, 0), 0xFFFF00))
#define N_PCI_CLASS_PROGIF(c, s, p) N_HAL_RULE(N_PCI_THE_BUS, N_HAL_NUMBER(N_PROP_PCI_CLASS, N_PCI_CLASS_CODE(c, s, p)))
#define N_PCI_ID_CLASS(v, d, c, s)                                                                                 \
    N_HAL_RULE(N_PCI_THE_BUS, N_HAL_NUMBER(N_PROP_PCI_VENDOR, v), N_HAL_NUMBER(N_PROP_PCI_DEVICE, d),              \
               N_HAL_MASKED(N_PROP_PCI_CLASS, N_PCI_CLASS_CODE(c, s, 0), 0xFFFF00))
#define N_PCI_VENDOR_CLASS_PROGIF(v, c, s, p)                                                                      \
    N_HAL_RULE(N_PCI_THE_BUS, N_HAL_NUMBER(N_PROP_PCI_VENDOR, v), N_HAL_NUMBER(N_PROP_PCI_CLASS, N_PCI_CLASS_CODE(c, s, p)))

/* USB の装置と、その interface。interface は装置の子である。 */
#define N_BUS_USB N_HAL_KEY('u', 's', 'b', 0)
#define N_BUS_USB_INTERFACE N_HAL_KEY('u', 's', 'i', 0)
#define N_PROP_USB_VENDOR N_HAL_KEY('u', 's', 'b', 1)
#define N_PROP_USB_PRODUCT N_HAL_KEY('u', 's', 'b', 2)
#define N_PROP_USB_RELEASE N_HAL_KEY('u', 's', 'b', 3)
#define N_PROP_USB_CLASS N_HAL_KEY('u', 's', 'b', 4) /* 24 bit：class、subclass、protocol。装置か interface の */
#define N_PROP_USB_INTERFACE N_HAL_KEY('u', 's', 'b', 5)
#define N_USB_CLASS_CODE(c, s, p) ((uint64_t)(c) << 16 | (uint64_t)(s) << 8 | (uint64_t)(p))
#define N_USB_INTERFACE_CLASS(c)                                                                                   \
    N_HAL_RULE(N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_USB_INTERFACE), N_HAL_MASKED(N_PROP_USB_CLASS, N_USB_CLASS_CODE(c, 0, 0), 0xFF0000))
#define N_USB_INTERFACE_SUBCLASS(c, s)                                                                             \
    N_HAL_RULE(N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_USB_INTERFACE), N_HAL_MASKED(N_PROP_USB_CLASS, N_USB_CLASS_CODE(c, s, 0), 0xFFFF00))

/* ACPI の名前空間の装置。_HID と _CID は字のバイトで、_CID は複数ありうる。 */
#define N_BUS_ACPI N_HAL_KEY('a', 'c', 'p', 0)
#define N_PROP_ACPI_ID N_HAL_KEY('a', 'c', 'p', 1)
#define N_PROP_ACPI_UID N_HAL_KEY('a', 'c', 'p', 2)
#define N_ACPI_ID(hid) N_HAL_RULE(N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_ACPI), N_HAL_TEXT(N_PROP_ACPI_ID, hid))

/* VMBus の channel。型は GUID の 16 バイト。 */
#define N_BUS_VMBUS N_HAL_KEY('v', 'm', 'b', 0)
#define N_PROP_VMBUS_TYPE N_HAL_KEY('v', 'm', 'b', 1)
#define N_VMBUS_TYPE(guid) N_HAL_RULE(N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_VMBUS), N_HAL_BYTES(N_PROP_VMBUS_TYPE, guid, 16))

/* 機械が最初から持つ物：firmware が渡した画面、決まった番地のポート。名前で引く。 */
#define N_BUS_FIXED N_HAL_KEY('f', 'i', 'x', 0)
#define N_PROP_FIXED_NAME N_HAL_KEY('f', 'i', 'x', 1)
#define N_FIXED(name) N_HAL_RULE(N_HAL_NUMBER(N_HAL_PROP_BUS, N_BUS_FIXED), N_HAL_TEXT(N_PROP_FIXED_NAME, name))

/* 実装が提供するものの種類（struct n_hal_offer）。 */
#define N_OFFER_DISK N_HAL_KEY('o', 'f', 'd', 1)
#define N_OFFER_NET N_HAL_KEY('o', 'f', 'n', 1)
#define N_OFFER_AUDIO N_HAL_KEY('o', 'f', 'a', 1)
#define N_OFFER_INPUT N_HAL_KEY('o', 'f', 'i', 1)
#define N_OFFER_SCREEN N_HAL_KEY('o', 'f', 's', 1)
#define N_OFFER_GPU N_HAL_KEY('o', 'f', 'g', 1)
