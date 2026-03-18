/*
 * usb.h - USB Protocol Types
 *
 * Standard USB 2.0/3.x descriptor structures, request codes, and
 * HID class definitions used by the xHCI driver and HID keyboard layer.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Descriptor types                                                    */
/* ------------------------------------------------------------------ */

#define USB_DESC_DEVICE         0x01u
#define USB_DESC_CONFIG         0x02u
#define USB_DESC_STRING         0x03u
#define USB_DESC_INTERFACE      0x04u
#define USB_DESC_ENDPOINT       0x05u
#define USB_DESC_HID            0x21u

/* ------------------------------------------------------------------ */
/* bmRequestType fields                                                */
/* ------------------------------------------------------------------ */

#define USB_DIR_OUT             0x00u
#define USB_DIR_IN              0x80u

#define USB_TYPE_STANDARD       0x00u
#define USB_TYPE_CLASS          0x20u
#define USB_TYPE_VENDOR         0x40u

#define USB_RECIP_DEVICE        0x00u
#define USB_RECIP_INTERFACE     0x01u
#define USB_RECIP_ENDPOINT      0x02u
#define USB_RECIP_OTHER         0x03u

/* ------------------------------------------------------------------ */
/* Standard request codes                                              */
/* ------------------------------------------------------------------ */

#define USB_REQ_GET_STATUS          0x00u
#define USB_REQ_CLEAR_FEATURE       0x01u
#define USB_REQ_SET_FEATURE         0x03u
#define USB_REQ_SET_ADDRESS         0x05u
#define USB_REQ_GET_DESCRIPTOR      0x06u
#define USB_REQ_SET_DESCRIPTOR      0x07u
#define USB_REQ_GET_CONFIGURATION   0x08u
#define USB_REQ_SET_CONFIGURATION   0x09u
#define USB_REQ_GET_INTERFACE       0x0Au
#define USB_REQ_SET_INTERFACE       0x0Bu

/* ------------------------------------------------------------------ */
/* HID class request codes                                             */
/* ------------------------------------------------------------------ */

#define USB_HID_REQ_GET_REPORT      0x01u
#define USB_HID_REQ_GET_IDLE        0x02u
#define USB_HID_REQ_GET_PROTOCOL    0x03u
#define USB_HID_REQ_SET_REPORT      0x09u
#define USB_HID_REQ_SET_IDLE        0x0Au
#define USB_HID_REQ_SET_PROTOCOL    0x0Bu

#define USB_HID_PROTOCOL_BOOT       0x00u
#define USB_HID_PROTOCOL_REPORT     0x01u

/* ------------------------------------------------------------------ */
/* Class codes                                                         */
/* ------------------------------------------------------------------ */

#define USB_CLASS_HID               0x03u
#define USB_HID_SUBCLASS_BOOT       0x01u
#define USB_HID_PROTOCOL_KEYBOARD   0x01u
#define USB_HID_PROTOCOL_MOUSE      0x02u

/* ------------------------------------------------------------------ */
/* Endpoint direction / type                                           */
/* ------------------------------------------------------------------ */

#define USB_EP_DIR_OUT          0x00u
#define USB_EP_DIR_IN           0x80u
#define USB_EP_TYPE_CONTROL     0x00u
#define USB_EP_TYPE_ISOCHRONOUS 0x01u
#define USB_EP_TYPE_BULK        0x02u
#define USB_EP_TYPE_INTERRUPT   0x03u

/* ------------------------------------------------------------------ */
/* Setup packet (8 bytes, sent in Setup Stage TRB immediate data)     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;

/* ------------------------------------------------------------------ */
/* USB Descriptors                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;   /* USB_DESC_DEVICE */
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;   /* USB_DESC_CONFIG */
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;   /* USB_DESC_INTERFACE */
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;   /* USB_DESC_ENDPOINT */
    uint8_t  bEndpointAddress;  /* [3:0]=EP number, [7]=direction */
    uint8_t  bmAttributes;      /* [1:0]=transfer type */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;         /* Polling interval in frames/microframes */
} __attribute__((packed)) usb_endpoint_desc_t;

/* ------------------------------------------------------------------ */
/* HID Boot Keyboard Input Report (8 bytes)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t modifiers;  /* Bit flags: LCTRL LSHIFT LALT LGUI RCTRL RSHIFT RALT RGUI */
    uint8_t reserved;
    uint8_t keycodes[6];
} __attribute__((packed)) usb_hid_boot_report_t;
