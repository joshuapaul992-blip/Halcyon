/* Halcyon OS - USB core
 *
 * Layering:
 *   host controller drivers (uhci.c, ohci.c, ehci.c, xhci.c)
 *        |  usb_hc_ops_t (below), usb_register_hc(), usb_attach_device()
 *   USB core (usb.c): enumeration, descriptors, addresses, driver binding
 *        |  usb_driver_t
 *   class drivers: hub.c, hid.c (keyboard/mouse/tablet), msc.c (mass storage)
 *
 * Everything is polled from usb_poll() (main loop). Control and bulk
 * transfers are synchronous; interrupt-IN transfers are asynchronous
 * (intr_start + intr_poll) so HID devices and hubs can be serviced without
 * blocking.
 *
 * Memory: every kernel pointer is identity mapped and heap/stack/bss memory
 * lies below 4 GiB, so HC drivers can DMA directly from/to caller buffers
 * (no alignment guarantees: drivers handle arbitrary byte alignment).
 */
#ifndef USB_H
#define USB_H
#include "core/types.h"
#include "core/irq.h"
#include "drivers/pci.h"

/* ---------- standard definitions ---------- */
enum usb_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW = 1,        /* 1.5 Mb/s  */
    USB_SPEED_FULL = 2,       /* 12 Mb/s   */
    USB_SPEED_HIGH = 3,       /* 480 Mb/s  */
    USB_SPEED_SUPER = 4,      /* 5 Gb/s    */
    USB_SPEED_SUPERPLUS = 5,  /* 10+ Gb/s  */
};

#define USB_DIR_OUT 0x00
#define USB_DIR_IN  0x80
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS    0x20
#define USB_TYPE_VENDOR   0x40
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02
#define USB_RECIP_OTHER     0x03

#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_DESCRIPTOR    7
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE     11
#define USB_REQ_SET_SEL           48

#define USB_DT_DEVICE        1
#define USB_DT_CONFIG        2
#define USB_DT_STRING        3
#define USB_DT_INTERFACE     4
#define USB_DT_ENDPOINT      5
#define USB_DT_DEVICE_QUALIFIER 6
#define USB_DT_HID           0x21
#define USB_DT_REPORT        0x22
#define USB_DT_HUB           0x29
#define USB_DT_SS_HUB        0x2A
#define USB_DT_SS_EP_COMPANION 0x30

#define USB_FEATURE_ENDPOINT_HALT 0

#define USB_EP_CONTROL 0
#define USB_EP_ISO     1
#define USB_EP_BULK    2
#define USB_EP_INTR    3

#define USB_CLASS_PER_INTERFACE 0x00
#define USB_CLASS_AUDIO   0x01
#define USB_CLASS_CDC     0x02
#define USB_CLASS_HID     0x03
#define USB_CLASS_PRINTER 0x07
#define USB_CLASS_MSC     0x08
#define USB_CLASS_HUB     0x09
#define USB_CLASS_VIDEO   0x0E
#define USB_CLASS_WIRELESS 0xE0
#define USB_CLASS_MISC    0xEF
#define USB_CLASS_VENDOR  0xFF

typedef struct PACKED usb_setup {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} usb_setup_t;

typedef struct PACKED usb_device_desc {
    u8  bLength, bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    u16 idVendor, idProduct, bcdDevice;
    u8  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_device_desc_t;

typedef struct PACKED usb_config_desc {
    u8  bLength, bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} usb_config_desc_t;

typedef struct PACKED usb_interface_desc {
    u8 bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} usb_interface_desc_t;

typedef struct PACKED usb_endpoint_desc {
    u8  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} usb_endpoint_desc_t;

/* ---------- core objects ---------- */
typedef struct usb_hc usb_hc_t;
typedef struct usb_device usb_device_t;
typedef struct usb_driver usb_driver_t;

typedef struct usb_endpoint {
    u8  address;        /* bEndpointAddress: number | 0x80 for IN (ep0: 0) */
    u8  type;           /* USB_EP_* */
    u16 mps;            /* max packet size (bits 0..10 of wMaxPacketSize) */
    u8  mult;           /* HS high-bandwidth: extra transactions per microframe (bits 11..12) */
    u8  interval;       /* raw bInterval */
    u8  max_burst;      /* SuperSpeed companion bMaxBurst (0 if none) */
    u8  toggle;         /* data toggle, for HCs that track it in software */
    u8  interface;      /* bInterfaceNumber this endpoint belongs to */
    void *hc_priv;      /* HC driver per-endpoint state */
} usb_endpoint_t;

#define USB_MAX_INTERFACES 8

typedef struct usb_binding {
    const usb_driver_t *driver;
    u8 interface;          /* bInterfaceNumber */
    void *priv;            /* driver state */
} usb_binding_t;

struct usb_device {
    usb_hc_t *hc;
    usb_device_t *parent;  /* hub this device hangs off, NULL for a root-hub port */
    u8  port;              /* 1-based port number on parent (or on the root hub) */
    u8  root_port;         /* 1-based root hub port at the top of this branch */
    u8  speed;             /* USB_SPEED_* */
    u8  address;           /* USB device address, 0 until assigned */
    u8  hub_depth;         /* number of hubs between the root hub and this device */
    u32 route;             /* xHCI route string (4 bits per hub tier) */
    usb_device_t *tt_hub;  /* LS/FS device behind a HS hub: the HS hub owning the TT */
    u8  tt_port;           /* port of tt_hub the LS/FS subtree is attached to */

    /* hub info (filled by the hub driver before hc->ops->hub_init) */
    u8  is_hub;
    u8  hub_nports;
    u8  hub_multi_tt;      /* hub has one TT per port */
    u8  hub_ttt;           /* TT think time code 0..3 (wHubCharacteristics bits 5..6) */

    usb_endpoint_t ep0;
    usb_endpoint_t *eps[32];   /* index: endpoint number + (IN ? 16 : 0); [0] unused */

    usb_device_desc_t desc;
    u8  *config;               /* full configuration descriptor */
    u16 config_len;
    char manufacturer[64];
    char product[64];
    char serial[64];
    char description[64];      /* "USB Keyboard", "Mass Storage (sda)", ... */

    usb_binding_t bind[USB_MAX_INTERFACES];
    int nbind;

    void *hc_priv;             /* HC per-device state (e.g. xHCI slot) */
    u8  alive;
    u8  id;                    /* small unique number for naming */
    usb_device_t *next;        /* global device list */
};

/* Host controller interface. All functions are called from the single
 * kernel thread. Return values: >=0 success (bytes for transfers), negative
 * E_* on failure (E_STALL for a STALL handshake, E_TIMEOUT, E_IO, E_NODEV
 * when the device is gone). */
typedef struct usb_hc_ops {
    /* Prepare a freshly reset device for its first control transfer.
     * Valid on entry: dev->hc, parent, port, root_port, speed, hub_depth,
     * route, tt_hub, tt_port; dev->address == 0; dev->ep0.mps is a guess
     * (8 LS, 64 FS/HS, 512 SS). HCs with USB_HC_ASSIGNS_ADDRESS (xHCI) must
     * assign the address here (Address Device) and store it in dev->address. */
    int  (*device_init)(usb_hc_t *hc, usb_device_t *dev);
    /* dev->address and/or dev->ep0.mps changed (after SET_ADDRESS or after
     * reading bMaxPacketSize0). xHCI: Evaluate Context for ep0 mps. */
    int  (*device_update)(usb_hc_t *hc, usb_device_t *dev);
    /* release all HC state of a device that is gone or failed enumeration
     * (xHCI: Disable Slot). Must cope with partially initialised devices. */
    void (*device_free)(usb_hc_t *hc, usb_device_t *dev);
    /* synchronous control transfer on endpoint 0. data may be NULL when
     * wLength == 0. Returns the number of data-stage bytes transferred. */
    int  (*control)(usb_hc_t *hc, usb_device_t *dev, const usb_setup_t *setup, void *data, int timeout_ms);
    /* the core parsed the configuration: set up every non-NULL dev->eps[]
     * (xHCI: Configure Endpoint command). Called before SET_CONFIGURATION. */
    int  (*configure)(usb_hc_t *hc, usb_device_t *dev);
    /* synchronous bulk transfer of any length (the driver splits it).
     * Returns bytes transferred (short packets end the transfer early). */
    int  (*bulk)(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms);
    /* asynchronous interrupt transfer (IN, or OUT): queue one transfer of up
     * to len bytes (len <= ep->mps * (mult+1) * (max_burst+1)). */
    int  (*intr_start)(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len);
    /* E_AGAIN while pending; >=0 bytes when completed (the transfer is then
     * retired and intr_start may be called again); <0 on error (retired). */
    int  (*intr_poll)(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep);
    /* abort a pending interrupt transfer (no-op if none) */
    void (*intr_cancel)(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep);
    /* endpoint halt was cleared by the core (CLEAR_FEATURE sent): reset the
     * data toggle to DATA0 / xHCI Reset Endpoint + Set TR Dequeue Pointer */
    int  (*reset_endpoint)(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep);
    /* dev is a hub: hub_nports/hub_multi_tt/hub_ttt are valid (xHCI: update
     * slot context Hub/NumPorts/TTT/MTT). Optional (may be NULL). */
    int  (*hub_init)(usb_hc_t *hc, usb_device_t *dev);
    /* root hub service, called from usb_poll() every few ms. Detect
     * connect/disconnect on root ports, debounce, reset, and call
     * usb_attach_device() / usb_port_disconnected(). */
    void (*poll)(usb_hc_t *hc);
} usb_hc_ops_t;

#define USB_HC_ASSIGNS_ADDRESS 0x01    /* xHCI: SET_ADDRESS is done by the HC */

struct usb_hc {
    const char *name;          /* "UHCI", "OHCI", "EHCI", "xHCI" */
    char desc[64];             /* "Intel UHCI (00:1d.0)" */
    const usb_hc_ops_t *ops;
    void *priv;
    pci_dev_t *pci;
    u32 flags;                 /* USB_HC_* */
    u8 nports;                 /* root hub ports */
    u8 id;
    u8 addr_used[128];         /* address allocator (core) */
    /* interrupts: the HC driver fills in irq.name/pci/ack and calls
     * irq_attach(); until irq_live() the core polls the HC */
    irq_handle_t irq;
    u8 no_port_irq;            /* root port changes are not signalled (UHCI) */
    u64 last_poll;             /* core */
    usb_hc_t *next;
};

/* class drivers bind per interface */
struct usb_driver {
    const char *name;
    /* return 0 to claim the interface; store state in b->priv */
    int  (*probe)(usb_device_t *dev, const usb_interface_desc_t *intf, usb_binding_t *b);
    void (*poll)(usb_device_t *dev, usb_binding_t *b);
    void (*disconnect)(usb_device_t *dev, usb_binding_t *b);
};

/* ---------- core API ---------- */
void usb_init(void);
void usb_poll(void);

void usb_register_hc(usb_hc_t *hc);
int  usb_hc_irq(usb_hc_t *hc, int (*ack)(void *ctx), void *ctx);

/* Called by HC drivers (root ports) and the hub driver after a port reset
 * completed with a device present. Enumerates and binds drivers.
 * Returns the device or NULL if enumeration failed. */
usb_device_t *usb_attach_device(usb_hc_t *hc, usb_device_t *parent, int port, int speed);
/* The device on (parent, port) disappeared: detach it (and its subtree). */
void usb_port_disconnected(usb_hc_t *hc, usb_device_t *parent, int port);
usb_device_t *usb_find_child(usb_hc_t *hc, usb_device_t *parent, int port);

/* transfers */
int usb_control(usb_device_t *dev, u8 reqtype, u8 req, u16 value, u16 index, void *data, u16 len);
int usb_control_timeout(usb_device_t *dev, u8 reqtype, u8 req, u16 value, u16 index, void *data, u16 len, int timeout_ms);
int usb_get_descriptor(usb_device_t *dev, u8 type, u8 index, u16 lang, void *buf, u16 len);
int usb_bulk(usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms);
int usb_clear_halt(usb_device_t *dev, usb_endpoint_t *ep);
int usb_intr_start(usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len);
int usb_intr_poll(usb_device_t *dev, usb_endpoint_t *ep);
void usb_intr_cancel(usb_device_t *dev, usb_endpoint_t *ep);

/* find the first endpoint of an interface with the given type and direction (USB_DIR_IN/OUT) */
usb_endpoint_t *usb_find_endpoint(usb_device_t *dev, u8 interface, u8 type, u8 dir);

/* introspection for the GUI */
usb_device_t *usb_first_device(void);
usb_hc_t *usb_first_hc(void);
u32 usb_generation(void);       /* increments on attach/detach */
const char *usb_speed_name(int speed);
const char *usb_class_name(u8 cls);

/* helpers for HC drivers */
static inline int usb_ep_is_in(const usb_endpoint_t *ep) { return (ep->address & 0x80) != 0; }
static inline int usb_ep_num(const usb_endpoint_t *ep) { return ep->address & 0x0F; }

/* class driver registration (called from usb_init) */
extern const usb_driver_t usb_hub_driver;
extern const usb_driver_t usb_hid_driver;
extern const usb_driver_t usb_msc_driver;

#endif
