#ifndef _USB_CONFIG_H
#define _USB_CONFIG_H

#include "funconfig.h"
#include "ch32fun.h"

// エンドポイント構成は ch32fun examples_v30x/usbdevice_hs の実績構成を踏襲:
//   EP0: コントロール / EP4: バルクIN (デバイス→PC) / EP5: バルクOUT (PC→デバイス)
// EP1-3 はスタックの既定 (TX) のまま未使用。HID インターフェースは持たない。
#define HUSB_CONFIG_EPS       6 // EP0 を含む
#define HUSB_SUPPORTS_SLEEP   0
#define HUSB_HID_INTERFACES   0
#define HUSB_HID_USER_REPORTS 0
#define HUSB_IO_PROFILE       1
#define HUSB_BULK_USER_REPORTS 1

#include "usb_defines.h"

static const uint8_t device_descriptor[] = {
	18,         // bLength
	1,          // bDescriptorType (Device)
	0x00, 0x02, // bcdUSB 2.00
	0xff,       // bDeviceClass (Vendor)
	0x00,       // bDeviceSubClass
	0x00,       // bDeviceProtocol
	64,         // bMaxPacketSize0
	0x09, 0x12, // idVendor  0x1209 (pid.codes)
	0x01, 0xc0, // idProduct 0xC001
	0x10, 0x00, // bcdDevice 0.10 (M0.5)
	1,          // iManufacturer
	2,          // iProduct
	3,          // iSerialNumber
	1,          // bNumConfigurations
};

static const uint8_t config_descriptor[] = {
	/* Configuration Descriptor */
	0x09,       // bLength
	0x02,       // bDescriptorType (Configuration)
	0x20, 0x00, // wTotalLength (9+9+7+7 = 32)
	0x01,       // bNumInterfaces
	0x01,       // bConfigurationValue
	0x00,       // iConfiguration
	0x80,       // bmAttributes: Bus Powered
	0xfa,       // MaxPower: 500mA (GBAと共存するため大きめに宣言)

	/* Interface Descriptor (Vendor Bulk) */
	0x09,       // bLength
	0x04,       // bDescriptorType (Interface)
	0x00,       // bInterfaceNumber
	0x00,       // bAlternateSetting
	0x02,       // bNumEndpoints
	0xff,       // bInterfaceClass (Vendor)
	0xff,       // bInterfaceSubClass
	0xff,       // bInterfaceProtocol
	0x00,       // iInterface

	/* Endpoint Descriptor: EP4 IN (Bulk, 512B) */
	0x07,       // bLength
	0x05,       // bDescriptorType (Endpoint)
	0x84,       // bEndpointAddress: IN 4
	0x02,       // bmAttributes: Bulk
	0x00, 0x02, // wMaxPacketSize: 512
	0x00,       // bInterval

	/* Endpoint Descriptor: EP5 OUT (Bulk, 512B) */
	0x07,       // bLength
	0x05,       // bDescriptorType (Endpoint)
	0x05,       // bEndpointAddress: OUT 5
	0x02,       // bmAttributes: Bulk
	0x00, 0x02, // wMaxPacketSize: 512
	0x00,       // bInterval
};

#define STR_MANUFACTURER u"pit-true"
#define STR_PRODUCT      u"NisekoroCapture"
#define STR_SERIAL       u"GBACAP-001"

struct usb_string_descriptor_struct {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t wString[];
};
const static struct usb_string_descriptor_struct string0 __attribute__((section(".rodata"))) = {
	4, 3, {0x0409}
};
const static struct usb_string_descriptor_struct string1 __attribute__((section(".rodata"))) = {
	sizeof(STR_MANUFACTURER), 3, STR_MANUFACTURER
};
const static struct usb_string_descriptor_struct string2 __attribute__((section(".rodata"))) = {
	sizeof(STR_PRODUCT), 3, STR_PRODUCT
};
const static struct usb_string_descriptor_struct string3 __attribute__((section(".rodata"))) = {
	sizeof(STR_SERIAL), 3, STR_SERIAL
};

const static struct descriptor_list_struct {
	uint32_t	lIndexValue;
	const uint8_t	*addr;
	uint8_t		length;
} descriptor_list[] = {
	{0x00000100, device_descriptor, sizeof(device_descriptor)},
	{0x00000200, config_descriptor, sizeof(config_descriptor)},
	{0x00000300, (const uint8_t *)&string0, 4},
	{0x04090301, (const uint8_t *)&string1, sizeof(STR_MANUFACTURER)},
	{0x04090302, (const uint8_t *)&string2, sizeof(STR_PRODUCT)},
	{0x04090303, (const uint8_t *)&string3, sizeof(STR_SERIAL)},
};
#define DESCRIPTOR_LIST_ENTRIES ((sizeof(descriptor_list))/(sizeof(struct descriptor_list_struct)) )

#endif
