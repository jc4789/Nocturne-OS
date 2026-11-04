/* 知られたバスの名前と性質の鍵（hal_abi.h）。バスを足すのは、ここに名前を足し、装置を hal_publish することで
   ある。HAL のコードは変わらない。カーネルの実装の規則と、サービスの宣言が同じ形を使う。 */
#pragma once
#include "abi.h"
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

/* PCI の claim のバスだけの操作（n_hal_bus_call）。番号は N_PCI_CALL_*、入出力は下の型。アドレスはバスの番地で、
   物理番地である：IOMMU が機能を隔離する（N_PCI_ISOLATED）なら、その domain は DMA の object を同じ番地に写す。 */
#define N_PCI_CALL_INFO 1             /* out: struct n_hal_pci_info。戻りはその大きさ */
#define N_PCI_CALL_CONFIG_READ 2      /* in: struct n_hal_pci_config。戻りは読んだ値 */
#define N_PCI_CALL_CONFIG_WRITE 3     /* in: struct n_hal_pci_config（value を書く） */
#define N_PCI_CALL_HOLD_SCREEN 4      /* in: uint32_t（0：返す、1：保持する） */
#define N_PCI_CALL_ACPI_EVAL 5        /* in: struct n_hal_pci_acpi と引数と data。out: struct n_acpi_value の並びと data。戻りは書いた数 */
#define N_PCI_CALL_ACPI_NOTIFY_TAKE 6 /* in: uint64_t の deadline。戻りは次の Notify の値 */

#define N_PCI_ISOLATED 1u       /* n_hal_pci_info.flags: DMA は claim の DMA の object にしか届かない */
#define N_PCI_NEEDS_RECOVERY 2u /* 前のドライバーは動いたまま終わった。装置の回復がドライバーの仕事 */
#define N_PCI_HAS_UPSTREAM 4u   /* 上にブリッジがある：n_hal_pci_info.upstream */
#define N_PCI_BAR_MEMORY 1u
#define N_PCI_BAR_64BIT 2u
#define N_PCI_BAR_PREFETCH 4u
#define N_PCI_BAR_IO 8u
#define N_PCI_NONE 0xFFFFFFFFu /* その BAR や画面は無い */
/* 機能の住所を一語で：32 bit の全部が住所である。 */
#define N_PCI_TARGET(segment, bus, dev, func)     ((uint32_t)(segment) << 16 | (uint32_t)(bus) << 8 | (uint32_t)(dev) << 3 | (uint32_t)(func))
#define N_PCI_TARGET_SEGMENT(target) ((uint16_t)((target) >> 16))
#define N_PCI_TARGET_BUS(target) ((uint8_t)((target) >> 8))
#define N_PCI_TARGET_DEV(target) ((uint8_t)((target) >> 3 & 0x1f))
#define N_PCI_TARGET_FUNC(target) ((uint8_t)((target) & 7))
#define N_PCI_SELF (UINT64_C(1) << 32) /* struct n_hal_pci_config の target：持った機能そのもの。N_PCI_TARGET ではない */
struct n_hal_pci_bar {
    uint64_t address, size;
    uint32_t flags, reserved;
};
struct n_hal_pci_info {
    uint16_t segment;
    uint8_t bus, dev, func, revision;
    uint16_t vendor, device, subsystem_vendor, subsystem_id;
    uint8_t cls, subcls, progif, interrupt_pin;
    uint32_t msi_vectors, msix_vectors; /* 機能が持つ数。無ければ 0 */
    uint32_t upstream;                  /* N_PCI_HAS_UPSTREAM のとき、上のブリッジの N_PCI_TARGET */
    uint32_t flags;                     /* N_PCI_ISOLATED、N_PCI_NEEDS_RECOVERY、N_PCI_HAS_UPSTREAM */
    struct n_hal_pci_bar bar[6];
    /* firmware が組んだ画面の画素がこの機能の BAR にあるとき（framebuffer_bar、無ければ N_PCI_NONE）、
       ドライバーはそのメモリーをそのままにする。 */
    uint64_t framebuffer_address, framebuffer_size;
    uint32_t framebuffer_bar, framebuffer_width, framebuffer_height, framebuffer_pitch;
    uint32_t framebuffer_bpp, reserved2;
};
struct n_hal_pci_config {
    uint64_t target; /* N_PCI_SELF か N_PCI_TARGET。書きは N_PCI_SELF だけ */
    uint32_t offset, width, value, reserved;
};
/* N_PCI_CALL_ACPI_EVAL の入力：この構造体、nargs 個の struct n_acpi_value、data_size バイトの data が続く。node は
   N_ACPI_SELF（機能の node）、N_ACPI_UPSTREAM（上のブリッジの node。root bus ならホストブリッジの）、
   N_ACPI_CHILD(i)（機能の node の i 番目の子）。name は名前の一区画（4 文字、"_DSM" など）。引数は整数、
   文字列、バッファーで、中身は data の中にある。 */
struct n_hal_pci_acpi {
    uint32_t node, nargs;
    char name[4];
    uint32_t data_size;
};

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
