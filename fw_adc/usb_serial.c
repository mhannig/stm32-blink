

/*
 * Initialize USB ACM serial device, provide convenience
 * function for serial communication.
 *
 * Mostly plucked together from some opencm3 example code.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>
#include <libopencm3/cm3/nvic.h>

#include "usb_serial.h"

#define STATUS_LED_PORT GPIOC
#define STATUS_LED_PIN  GPIO13

#define USBD_PORT GPIOA
#define USBDM     GPIO11
#define USBDP     GPIO12

#define RX_ECHO     1
#define RX_BUF_LEN  256

static char    _rx_tmp[RX_BUF_LEN];
static char    _rx_buf[RX_BUF_LEN];

static size_t  _rx_tmp_len;
static uint8_t _rx_buf_ready;

static usbd_device* __USBDEV;

static const struct usb_device_descriptor device_descriptor = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_CDC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x0483,
	.idProduct = 0x5740,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};


static const struct usb_endpoint_descriptor comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x83,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 16,
	.bInterval = 255,
}};


static const struct usb_endpoint_descriptor data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x01,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x82,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
}};


static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 0,
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = 0,
		.bSubordinateInterface0 = 1,
	 },
};

static const struct usb_interface_descriptor comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 0,

	.endpoint = comm_endp,

	.extra = &cdcacm_functional_descriptors,
	.extralen = sizeof(cdcacm_functional_descriptors),
}};

static const struct usb_interface_descriptor data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = data_endp,
}};

static const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.altsetting = comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = data_iface,
}};

static const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 0x32,

	.interface = ifaces,
};


static const char *usb_strings[] = {
	"FooBar Inc.",
	"Nordfnord2000",
	"FNORD23",
};

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[128];

static int cdcacm_control_request(usbd_device *usbd_dev,
                                  struct usb_setup_data *req,
                                  uint8_t **buf,
                                  uint16_t *len,
                                  usbd_control_complete_callback *complete)
{

	switch (req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
		/*
		 * This Linux cdc_acm driver requires this to be implemented
		 * even though it's optional in the CDC spec, and we don't
		 * advertise it in the ACM functional descriptor.
		 */
		char local_buf[10];
		struct usb_cdc_notification *notif = (void *)local_buf;

		/* We echo signals back to host as notification. */
		notif->bmRequestType = 0xA1;
		notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
		notif->wValue = 0;
		notif->wIndex = 0;
		notif->wLength = 2;
		local_buf[8] = req->wValue & 3;
		local_buf[9] = 0;
		// usbd_ep_write_packet(0x83, buf, 10);
		return 1;
		}
	case USB_CDC_REQ_SET_LINE_CODING:
		if (*len < sizeof(struct usb_cdc_line_coding))
			return 0;
		return 1;
	}
	return 0;
}


static void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
	char buf[64];

	int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, 64);

	if (len) {
        // We received data, let's handle it
        for (uint8_t i = 0; i < len; i++) {
            // Discard newlines
            if (buf[i] == '\n') {
                continue;
            }

            // Packets are delimited by a CR
            if (buf[i] == '\r' || buf[i] == '\0') {
                // Copy contents into target buffer
                strncpy(_rx_buf, _rx_tmp, RX_BUF_LEN);

                // Mark as ready
                _rx_buf_ready = 1;

                // Clear tmp
                memset(_rx_tmp, 0, RX_BUF_LEN);
                _rx_tmp_len = 0;

                continue;
            }

            // Append incoming data to packet
            _rx_tmp[_rx_tmp_len] = buf[i];
            _rx_tmp_len++;
        }

        #if RX_ECHO == 1
		usbd_ep_write_packet(usbd_dev, 0x82, buf, len);
        #endif
	}
}


static void cdcacm_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
	usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_data_rx_cb);
	usbd_ep_setup(usbd_dev, 0x82, USB_ENDPOINT_ATTR_BULK, 64, NULL);
	usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

	usbd_register_control_callback(
        usbd_dev,
        USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
        USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
        cdcacm_control_request
    );
}


const char* usb_serial_rx()
{
    if(_rx_buf_ready) {
        _rx_buf_ready = 0;
        return _rx_buf;
    }

    return NULL;
}

size_t usb_serial_tx(const char *data, size_t len)
{
    size_t tx_total = 0;
    char tx_buf[64];

    while (tx_total < len) {
        size_t tx_len = len - tx_total;
        if (tx_len > 63) {
            tx_len = 63;
        }

        memset(tx_buf, 0, 64);
        strncpy(tx_buf, data + tx_total, 64);

        size_t res = usbd_ep_write_packet(__USBDEV, 0x82, tx_buf, tx_len);
        if (res == 0) {
            return tx_total; // Something failed
        }

        tx_total += res;
    }

    return tx_total;
}

/*
 * Initialize GPIO for D+ and status LED
 */
void usb_gpio_init()
{
    // Clock
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOC);

    // D+
	gpio_set_mode(USBD_PORT,
                  GPIO_MODE_OUTPUT_2_MHZ,
		          GPIO_CNF_OUTPUT_PUSHPULL,
                  USBDP);

    // LED
    gpio_set_mode(STATUS_LED_PORT,
                  GPIO_MODE_OUTPUT_2_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL,
                  STATUS_LED_PIN);

}

/*
 * Switch status led on / off
 */
inline void usb_status_led_toggle()
{
    gpio_toggle(STATUS_LED_PORT, STATUS_LED_PIN);
}


/*
 * Poll usb interface using TIM4
 */
void usb_timer_init()
{
    // Enable clock
    rcc_periph_clock_enable(RCC_TIM4);

    // Enable interrupts
    nvic_enable_irq(NVIC_TIM4_IRQ);
    nvic_set_priority(NVIC_TIM4_IRQ, 1);

    timer_reset(TIM4);

    // Edge aligned
    TIM4_CR1 |= TIM_CR1_CKD_CK_INT |
                TIM_CR1_CMS_EDGE |
                TIM_CR1_DIR_UP;

    TIM4_PSC = 32;
    TIM4_ARR = 65535;

    // Interrupts:
    //  - Update / Overflow Event (UIE)
    TIM4_DIER |= TIM_DIER_UIE;
}

void usb_timer_start()
{
    TIM4_CR1 |= TIM_CR1_CEN;
}


void tim4_isr()
{
    // Poll USBDEV
    usbd_poll(__USBDEV);

    // Blink status LED
    usb_status_led_toggle();
    TIM4_SR &= ~TIM_SR_UIF; // Clear flag
}


usbd_device *usb_serial_init()
{
	usbd_device *usbd_dev;

    // Initialize GPIO
    usb_gpio_init();

    // Initialize timers
    usb_timer_init();

    // Initialize buffers
    memset(_rx_tmp, 0, RX_BUF_LEN);
    memset(_rx_buf, 0, RX_BUF_LEN);

    _rx_tmp_len = 0;
    _rx_buf_ready = 0;

    // Pull down D+
    gpio_clear(USBD_PORT, USBDP);
	for (int i = 0; i < 0x10000; i++) {
		__asm__("nop");
    }
    gpio_set(USBD_PORT, USBDP);

    // Initialize usb device
	usbd_dev = usbd_init(&st_usbfs_v1_usb_driver,
                         &device_descriptor,
                         &config,
                         usb_strings, 3,
                         usbd_control_buffer, sizeof(usbd_control_buffer));

	usbd_register_set_config_callback(usbd_dev, cdcacm_set_config);

    // Make available
    __USBDEV = usbd_dev;

    // Pull down
    gpio_clear(USBD_PORT, USBDP);

    // Poll a bit
	for (int i = 0; i < 0x80000; i++) {
        usbd_poll(usbd_dev);
    }

    // Start polling
    usb_timer_start();

    return usbd_dev;
}


/*
 * Override _write and redirect stdout to usb
 */
int _write(int file, char* data, int len)
{
    if (file < 2) {
        return usb_serial_tx(data, len);
    }

    // Set error and return failure
    errno = EIO;
    return -1;
}
