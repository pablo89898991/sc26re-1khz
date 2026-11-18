/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Optional Xbox 360 (XInput) controller output over the USB cable.
 *
 * With sc26re/pad_out = 1 the controller adds a vendor interface with the
 * XInput layout (class FF / 5D / 01) to its USB configuration and a
 * Microsoft OS 2.0 descriptor set that gives that interface the compatible
 * ID "XUSB10", so Windows binds its own Xbox 360 controller driver (xusb22)
 * to it - no Microsoft VID/PID and no driver install. The Steam Controller
 * interfaces stay as they are (report meter, Steam, keyboard / mouse).
 *
 * The final pad state (after the remapper and the gyro joystick) is sent as
 * the 20-byte XInput report; the Steam pad report is then neutral apart from
 * the Steam and QAM buttons, so the game does not get the input twice.
 * The descriptors are chosen at power-on: changing pad_out needs a restart.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc.h>

#include "controller.h"
#include "haptics.h"
#include "sc26re_cfg.h"
#include "xinput.h"

LOG_MODULE_REGISTER(xinput, LOG_LEVEL_INF);

#define XINPUT_VENDOR_CODE 0x20U
#define XINPUT_REPORT_SIZE 20U
#define XINPUT_EP_MPS 32U

struct xinput_desc
{
	struct usb_if_descriptor if0;
	uint8_t xusb[17]; /* class specific descriptor of the Xbox 360 wired controller */
	struct usb_ep_descriptor in_ep;
	struct usb_ep_descriptor out_ep;
	struct usb_desc_header nil_desc;
} __packed;

static struct xinput_desc xi_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = 0xFF,
		.bInterfaceSubClass = 0x5D,
		.bInterfaceProtocol = 0x01,
		.iInterface = 0,
	},
	.xusb = { 0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00 },
	.in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_INTERRUPT,
		.wMaxPacketSize = sys_cpu_to_le16(XINPUT_EP_MPS),
		.bInterval = 1,
	},
	.out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x01,
		.bmAttributes = USB_EP_TYPE_INTERRUPT,
		.wMaxPacketSize = sys_cpu_to_le16(XINPUT_EP_MPS),
		.bInterval = 8,
	},
	.nil_desc = { .bLength = 0, .bDescriptorType = 0 },
};

static const struct usb_desc_header *xi_fs_desc[] = {
	(struct usb_desc_header *)&xi_desc.if0,
	(struct usb_desc_header *)&xi_desc.xusb,
	(struct usb_desc_header *)&xi_desc.in_ep,
	(struct usb_desc_header *)&xi_desc.out_ep,
	(struct usb_desc_header *)&xi_desc.nil_desc,
};

/* ---- Microsoft OS 2.0: compatible ID XUSB10 for the XInput interface ---- */

struct xi_msos
{
	struct msosv2_descriptor_set_header header;
	struct msosv2_configuration_subset_header config;
	struct msosv2_function_subset_header function;
	struct msosv2_compatible_id compatible_id;
} __packed;

static struct xi_msos xi_msos = {
	.header = {
		.wLength = sizeof(struct msosv2_descriptor_set_header),
		.wDescriptorType = MS_OS_20_SET_HEADER_DESCRIPTOR,
		.dwWindowsVersion = sys_cpu_to_le32(0x06030000UL), /* Windows 8.1 */
		.wTotalLength = sizeof(struct xi_msos),
	},
	.config = {
		.wLength = sizeof(struct msosv2_configuration_subset_header),
		.wDescriptorType = MS_OS_20_SUBSET_HEADER_CONFIGURATION,
		.bConfigurationValue = 0,
		.bReserved = 0,
		.wTotalLength = sizeof(struct xi_msos) - sizeof(struct msosv2_descriptor_set_header),
	},
	.function = {
		.wLength = sizeof(struct msosv2_function_subset_header),
		.wDescriptorType = MS_OS_20_SUBSET_HEADER_FUNCTION,
		.bFirstInterface = 0, /* set when the host asks */
		.bReserved = 0,
		.wSubsetLength = sizeof(struct msosv2_function_subset_header) + sizeof(struct msosv2_compatible_id),
	},
	.compatible_id = {
		.wLength = sizeof(struct msosv2_compatible_id),
		.wDescriptorType = MS_OS_20_FEATURE_COMPATIBLE_ID,
		.CompatibleID = { 'X', 'U', 'S', 'B', '1', '0', 0x00, 0x00 },
		.SubCompatibleID = { 0 },
	},
};

struct xi_bos
{
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_msos cap;
} __packed;

static struct xi_bos xi_bos = {
	.platform = {
		.bLength = sizeof(struct usb_bos_platform_descriptor) + sizeof(struct usb_bos_capability_msos),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		.bReserved = 0,
		/* MS OS 2.0 platform capability D8DD60DF-4589-4CC7-9CD2-659D9E648A9F */
		.PlatformCapabilityUUID = { 0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D,
		                            0x9E, 0x64, 0x8A, 0x9F },
	},
	.cap = {
		.dwWindowsVersion = sys_cpu_to_le32(0x06030000UL),
		.wMSOSDescriptorSetTotalLength = sys_cpu_to_le16(sizeof(struct xi_msos)),
		.bMS_VendorCode = XINPUT_VENDOR_CODE,
		.bAltEnumCode = 0x00,
	},
};

static int xi_msos_to_host(const struct usbd_context *const ctx, const struct usb_setup_packet *const setup,
                           struct net_buf *const buf)
{
	ARG_UNUSED(ctx);
	if(setup->bRequest == XINPUT_VENDOR_CODE && setup->wIndex == MS_OS_20_DESCRIPTOR_INDEX)
	{
		xi_msos.function.bFirstInterface = xi_desc.if0.bInterfaceNumber;
		net_buf_add_mem(buf, &xi_msos, MIN(net_buf_tailroom(buf), MIN(setup->wLength, sizeof(xi_msos))));
		return 0;
	}
	return -ENOTSUP;
}

USBD_DESC_BOS_VREQ_DEFINE(xi_bos_vreq, sizeof(xi_bos), &xi_bos, XINPUT_VENDOR_CODE, xi_msos_to_host, NULL);

/* ---- class ---- */

static atomic_t xi_enabled;
static atomic_t xi_in_busy;
static struct usbd_class_data *xi_cdata;
static uint8_t xi_last[XINPUT_REPORT_SIZE];
static bool xi_last_valid;
static bool xi_active; /* pad_out = 1 at power-on */
static struct k_spinlock xi_lock;

static int xi_submit_out(struct usbd_class_data *const c_data)
{
	struct net_buf *buf = usbd_ep_buf_alloc(c_data, xi_desc.out_ep.bEndpointAddress, XINPUT_EP_MPS);
	int err;

	if(buf == NULL)
	{
		return -ENOMEM;
	}
	err = usbd_ep_enqueue(c_data, buf);
	if(err)
	{
		net_buf_unref(buf);
	}
	return err;
}

static int xi_request(struct usbd_class_data *const c_data, struct net_buf *const buf, const int err)
{
	struct udc_buf_info *bi = (struct udc_buf_info *)net_buf_user_data(buf);
	uint8_t ep = bi->ep;

	if(ep == xi_desc.out_ep.bEndpointAddress && err == 0 && buf->len >= 5U && buf->data[0] == 0x00U &&
	   buf->data[1] == 0x08U)
	{
		/* rumble from the game: big (left) and small (right) motor */
		haptics_xinput_rumble(buf->data[3], buf->data[4]);
	}
	net_buf_unref(buf);
	if(ep == xi_desc.in_ep.bEndpointAddress)
	{
		atomic_set(&xi_in_busy, 0);
		if(err != 0)
		{
			xi_last_valid = false; /* resend */
		}
	}
	else if(ep == xi_desc.out_ep.bEndpointAddress && atomic_get(&xi_enabled) && err != -ECONNABORTED)
	{
		/* next rumble / LED report */
		(void)xi_submit_out(c_data);
	}
	return 0;
}

static void *xi_get_desc(struct usbd_class_data *const c_data, const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(speed);
	return xi_fs_desc;
}

static void xi_enable(struct usbd_class_data *const c_data)
{
	/* the endpoints were renumbered when the configuration was built */
	xi_desc.xusb[6] = xi_desc.in_ep.bEndpointAddress;
	xi_desc.xusb[13] = xi_desc.out_ep.bEndpointAddress;
	atomic_set(&xi_in_busy, 0);
	xi_last_valid = false;
	atomic_set(&xi_enabled, 1);
	(void)xi_submit_out(c_data);
	LOG_INF("XInput interface %u enabled", xi_desc.if0.bInterfaceNumber);
}

static void xi_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	atomic_set(&xi_enabled, 0);
}

static int xi_init(struct usbd_class_data *c_data)
{
	xi_cdata = c_data;
	return 0;
}

static int xi_control_to_host(struct usbd_class_data *c_data, const struct usb_setup_packet *const setup,
                              struct net_buf *const buf)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(setup);
	ARG_UNUSED(buf);
	errno = -ENOTSUP;
	return 0;
}

static int xi_control_to_dev(struct usbd_class_data *c_data, const struct usb_setup_packet *const setup,
                             const struct net_buf *const buf)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(setup);
	ARG_UNUSED(buf);
	return 0;
}

static struct usbd_class_api xi_api = {
	.request = xi_request,
	.get_desc = xi_get_desc,
	.enable = xi_enable,
	.disable = xi_disable,
	.init = xi_init,
	.control_to_host = xi_control_to_host,
	.control_to_dev = xi_control_to_dev,
};

USBD_DEFINE_CLASS(xinput_0, &xi_api, NULL, NULL);

/* ---- API ---- */

void xinput_setup(struct usbd_context *ctx, bool on)
{
	xi_active = on;
	if(!on)
	{
		return;
	}
	if(usbd_add_descriptor(ctx, &xi_bos_vreq) != 0 ||
	   usbd_device_set_bcd_usb(ctx, USBD_SPEED_FS, 0x0201) != 0)
	{
		LOG_ERR("XInput: MS OS 2.0 descriptor not added");
	}
}

const char *xinput_class_name(void)
{
	return "xinput_0";
}

bool xinput_enabled(void)
{
	return xi_active && atomic_get(&xi_enabled) != 0;
}

static int16_t clamp16(int32_t v)
{
	return (int16_t)CLAMP(v, -32767, 32767);
}

void xinput_send(const struct controller_report *r, uint32_t extra)
{
	static const struct
	{
		uint8_t bit;
		uint8_t byte;
		uint8_t mask;
	} map[] = {
		{ CONTROLLER_BUTTON_DPAD_UP, 2, 0x01 },        { CONTROLLER_BUTTON_DPAD_DOWN, 2, 0x02 },
		{ CONTROLLER_BUTTON_DPAD_LEFT, 2, 0x04 },      { CONTROLLER_BUTTON_DPAD_RIGHT, 2, 0x08 },
		{ CONTROLLER_BUTTON_MENU, 2, 0x10 },           { CONTROLLER_BUTTON_VIEW, 2, 0x20 },
		{ CONTROLLER_BUTTON_LEFT_STICK, 2, 0x40 },     { CONTROLLER_BUTTON_RIGHT_STICK, 2, 0x80 },
		{ CONTROLLER_BUTTON_LEFT_SHOULDER, 3, 0x01 },  { CONTROLLER_BUTTON_RIGHT_SHOULDER, 3, 0x02 },
		{ CONTROLLER_BUTTON_A, 3, 0x10 },              { CONTROLLER_BUTTON_B, 3, 0x20 },
		{ CONTROLLER_BUTTON_X, 3, 0x40 },              { CONTROLLER_BUTTON_Y, 3, 0x80 },
	};
	uint8_t rep[XINPUT_REPORT_SIZE] = { 0x00, XINPUT_REPORT_SIZE };
	struct net_buf *buf;
	k_spinlock_key_t key;

	if(!xinput_enabled() || xi_cdata == NULL)
	{
		return;
	}
	/* Xbox only: the whole pad; Steam + Xbox: only the Xbox actions */
	bool mirror = sc26re_cfg_get(SC26RE_CFG_PAD_OUT) == 1;
	uint32_t btn = (mirror ? r->buttons : 0U) | extra;

	for(size_t i = 0; i < ARRAY_SIZE(map); ++i)
	{
		if(btn & BIT(map[i].bit))
		{
			rep[map[i].byte] |= map[i].mask;
		}
	}
	rep[4] = (btn & BIT(CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK)) ? 255U
	                                                           : (mirror ? (uint8_t)(MAX(r->trigger_left, 0) >> 7) : 0U);
	rep[5] = (btn & BIT(CONTROLLER_BUTTON_RIGHT_TRIGGER_CLICK)) ? 255U
	                                                            : (mirror ? (uint8_t)(MAX(r->trigger_right, 0) >> 7) : 0U);
	if(mirror)
	{
		sys_put_le16((uint16_t)clamp16(r->stick_left_x), &rep[6]);
		sys_put_le16((uint16_t)clamp16(r->stick_left_y), &rep[8]);
		sys_put_le16((uint16_t)clamp16(r->stick_right_x), &rep[10]);
		sys_put_le16((uint16_t)clamp16(r->stick_right_y), &rep[12]);
	}

	key = k_spin_lock(&xi_lock);
	if((xi_last_valid && memcmp(rep, xi_last, sizeof(rep)) == 0) || atomic_get(&xi_in_busy))
	{
		k_spin_unlock(&xi_lock, key);
		return;
	}
	atomic_set(&xi_in_busy, 1);
	memcpy(xi_last, rep, sizeof(rep));
	xi_last_valid = true;
	k_spin_unlock(&xi_lock, key);

	buf = usbd_ep_buf_alloc(xi_cdata, xi_desc.in_ep.bEndpointAddress, sizeof(rep));
	if(buf == NULL)
	{
		atomic_set(&xi_in_busy, 0);
		xi_last_valid = false;
		return;
	}
	net_buf_add_mem(buf, rep, sizeof(rep));
	if(usbd_ep_enqueue(xi_cdata, buf) != 0)
	{
		net_buf_unref(buf);
		atomic_set(&xi_in_busy, 0);
		xi_last_valid = false;
	}
}

void xinput_neutral_pad(struct controller_report *r)
{
	if(!xinput_enabled() || sc26re_cfg_get(SC26RE_CFG_PAD_OUT) != 1)
	{
		return;
	}
	/* Steam keeps its own buttons; the game gets the pad through XInput */
	r->buttons &= BIT(CONTROLLER_BUTTON_STEAM) | BIT(CONTROLLER_BUTTON_QAM);
	r->trigger_left = r->trigger_right = 0;
	r->stick_left_x = r->stick_left_y = 0;
	r->stick_right_x = r->stick_right_y = 0;
	r->touchpad_left_x = r->touchpad_left_y = 0;
	r->touchpad_left_pressure = 0;
	r->touchpad_right_x = r->touchpad_right_y = 0;
	r->touchpad_right_pressure = 0;
}
