#include <stdio.h>
#include <stdlib.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VIRTUAL_DEVICE_VID 0x1234
#define VIRTUAL_DEVICE_PID 0x5678

#define PAD_VID 0x046d
#define PAD_PID 0xc21f
#define DEBUG 1

#define IOCTL(fd, request...) \
	do { \
		if (ioctl(fd, request) == -1) { \
			perror("ioctl " #request); \
			return -1; \
		} \
	} while (0)

#define SETUP_ABS(type, min, max, res, flat_) \
	do { \
		IOCTL(fd, UI_SET_ABSBIT, type); \
		struct uinput_abs_setup abs = { \
			.code = type, \
			.absinfo = { \
				.minimum = min, \
				.maximum = max, \
				.resolution = res, \
				.flat = flat_ \
			} \
		}; \
		IOCTL(fd, UI_ABS_SETUP, &abs); \
	} while (0)

int fd;
static int interface_claimed = 0;
static int kernel_driver_detached = 0;
static int uinput_created = 0;
libusb_device_handle *handle;
libusb_context *ctx;
static volatile sig_atomic_t stop_requested = 0;

typedef enum {
	DPAD_UP = 0x1,
	DPAD_DOWN = 0x2,
	DPAD_LEFT = 0x4,
	DPAD_RIGHT = 0x8,
	START = 0x10,
	BACK = 0x20,
	LEFT_STICK = 0x40,
	RIGHT_STICK = 0x80,
} BUTTONS_ONE;

typedef enum {
	LB = 0x1,
	RB = 0x2,
	LOGITECH = 0x4,
	A = 0x10,
	B = 0x20,
	X = 0x40,
	Y = 0x80,
} BUTTONS_TWO;

typedef enum {
	LEFT, RIGHT
} Trigger;

void emit(const int type, const int code, const int val) {
	const struct input_event ie = {
		.type = type,
		.code = code,
		.value = val,
		.time = {
			.tv_sec = 0,
			.tv_usec = 0
		}
	};

	write(fd, &ie, sizeof(ie));
}

static void disconnect_device(void) {
	if (handle == nullptr) {
		return;
	}

	if (interface_claimed) {
		libusb_release_interface(handle, 0);
		interface_claimed = 0;
	}

	if (kernel_driver_detached) {
		libusb_attach_kernel_driver(handle, 0);
		kernel_driver_detached = 0;
	}

	libusb_close(handle);
	handle = nullptr;
}

static int connect_to_device() {
	libusb_device **list;
	const ssize_t count = libusb_get_device_list(ctx, &list);
	if (count < 0) {
		fprintf(stderr, "libusb_get_device_list: %s\n", libusb_error_name((int)count));
		return -1;
	}

	for (ssize_t i = 0; i < count; i++) {
		libusb_device *device = list[i];

		struct libusb_device_descriptor desc;
		int ret = libusb_get_device_descriptor(device, &desc);
		if (ret != 0) continue;

		if (desc.idVendor != PAD_VID || desc.idProduct != PAD_PID) continue;

		ret = libusb_open(device, &handle);
		if (ret != 0) continue;

		if (libusb_kernel_driver_active(handle, 0) == 1) {
			ret = libusb_detach_kernel_driver(handle, 0);
			if (ret != 0) {
				fprintf(stderr, "libusb_detach_kernel_driver: %s\n", libusb_error_name(ret));
				libusb_close(handle);
				handle = nullptr;
				continue;
			}
			kernel_driver_detached = 1;
		}

		ret = libusb_claim_interface(handle, 0);
		if (ret != 0) {
			disconnect_device();
			continue;
		}

		interface_claimed = 1;
		libusb_free_device_list(list, 1);

		return 0;
	}
	libusb_free_device_list(list, 1);
	return -1;
}

static void destroy_uinput() {
	if (fd >= 0) {
		if (uinput_created) {
			ioctl(fd, UI_DEV_DESTROY);
			uinput_created = 0;
		}
		close(fd);
		fd = -1;
	}
}

static void on_signal(const int signal) {
	stop_requested = signal;
}

void cleanup() {
	disconnect_device();
	destroy_uinput();
	if (ctx != nullptr) {
		libusb_exit(ctx);
		ctx = nullptr;
	}
}

static int setup_uinput() {
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("open /dev/uinput");
		return -1;
	}
	IOCTL(fd, UI_SET_EVBIT, EV_KEY);
	constexpr int buttons[] = {BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST};

	constexpr size_t num_buttons = sizeof(buttons) / sizeof(buttons[0]);
	for (int i = 0; i < num_buttons; i++) {
		IOCTL(fd, UI_SET_KEYBIT, buttons[i]);
	}

	IOCTL(fd, UI_SET_EVBIT, EV_ABS);

	SETUP_ABS(ABS_X, -32768, 32767, 0, 0);
	SETUP_ABS(ABS_Y, -32768, 32767, 0, 0);
	SETUP_ABS(ABS_RX, -32768, 32767, 0, 0);
	SETUP_ABS(ABS_RY, -32768, 32767, 0, 0);
	SETUP_ABS(ABS_Z, 0, 255, 0, 0);
	SETUP_ABS(ABS_RZ, 0, 255, 0, 0);
	SETUP_ABS(ABS_HAT0X, -1, 1, 0, 0);
	SETUP_ABS(ABS_HAT0Y, -1, 1, 0, 0);

	struct uinput_setup usetup = {
		.id = {
			.bustype = BUS_USB,
			.vendor = VIRTUAL_DEVICE_VID,
			.product = VIRTUAL_DEVICE_PID,
		},
		.name = "Input handler"
	};

	IOCTL(fd, UI_DEV_SETUP, &usetup);
	IOCTL(fd, UI_DEV_CREATE);
	uinput_created = 1;
	return 0;
}

void setup_signal_handlers() {
	struct sigaction sa;
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGQUIT, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
}

static int prev_buttons_one = 0, prev_buttons_two = 0;
static int prev_left_stick_x = 0, prev_left_stick_y = 0;
static int prev_right_stick_x = 0, prev_right_stick_y = 0;
static int prev_left_trigger = 0, prev_right_trigger = 0;

void emit_data(const unsigned char data[]) {
	const int buttons_one = data[2];
	int dpad_x = 0;
	if (buttons_one & DPAD_LEFT) {
		// emit(EV_ABS, ABS_HAT0X, -1);
		dpad_x = -32768;
	} else if (buttons_one & DPAD_RIGHT) {
		// emit(EV_ABS, ABS_HAT0X, 1);
		dpad_x = 32767;
	} else {
		// emit(EV_ABS, ABS_HAT0X, 0);
	}
	if (buttons_one != prev_buttons_one) {
		if (buttons_one & DPAD_DOWN) {
			emit(EV_ABS, ABS_HAT0Y, 1);
		} else if (buttons_one & DPAD_UP) {
			emit(EV_ABS, ABS_HAT0Y, -1);
		} else {
			emit(EV_ABS, ABS_HAT0Y, 0);
		}

		emit(EV_KEY, BTN_START, (buttons_one & START) ? 1 : 0);
		emit(EV_KEY, BTN_BACK, (buttons_one & BACK) ? 1 : 0);
		emit(EV_KEY, BTN_THUMBL, (buttons_one & LEFT_STICK) ? 1 : 0);
		emit(EV_KEY, BTN_THUMBR, (buttons_one & RIGHT_STICK) ? 1 : 0);
	}
	const int buttons_two = data[3];
	if (buttons_two != prev_buttons_two) {
		emit(EV_KEY, BTN_TL, (buttons_two & LB) ? 1 : 0);
		emit(EV_KEY, BTN_TR, (buttons_two & RB) ? 1 : 0);
		emit(EV_KEY, BTN_MODE, (buttons_two & LOGITECH) ? 1 : 0);
		emit(EV_KEY, BTN_SOUTH, (buttons_two & A) ? 1 : 0);
		emit(EV_KEY, BTN_EAST, (buttons_two & B) ? 1 : 0);
		emit(EV_KEY, BTN_NORTH, (buttons_two & X) ? 1 : 0);
		emit(EV_KEY, BTN_WEST, (buttons_two & Y) ? 1 : 0);
	}
	// const int left_stick_x = ((data[6] << 8) | data[7]) - 32768;
	const int left_stick_y = ((data[8] << 8) | data[9]) - 32768;
	const int right_stick_x = ((data[10] << 8) | data[11]) - 32768;
	const int right_stick_y = ((data[12] << 8) | data[13]) - 32768;
	// if (left_stick_x != prev_left_stick_x) emit(EV_ABS, ABS_X, left_stick_x);
	if (left_stick_y != prev_left_stick_y) emit(EV_ABS, ABS_Y, left_stick_y * -1);
	if (right_stick_x != prev_right_stick_x) emit(EV_ABS, ABS_RX, right_stick_x);
	if (right_stick_y != prev_right_stick_y) emit(EV_ABS, ABS_RY, right_stick_y * -1);
	const int left_trigger = data[4] * 32767 / 255;
	const int right_trigger = data[5] * 32767 / 255;
	int final_left_stick_x;
	if (dpad_x != 0) {
		// Dpad maxes out the left stick for full left/right input, so LT/RT are ignored during this
		final_left_stick_x = dpad_x;
	} else {
		final_left_stick_x = right_trigger - left_trigger;
		if (abs(final_left_stick_x) <= 6) final_left_stick_x = 0; // Prevent "stick drift"
	}

	if (final_left_stick_x != prev_left_stick_x) {
		emit(EV_ABS, ABS_X, final_left_stick_x);
		prev_left_stick_x = final_left_stick_x;
	}

	emit(EV_SYN, SYN_REPORT, 0);

	prev_buttons_one = buttons_one;
	prev_buttons_two = buttons_two;
	// prev_left_stick_x = left_stick_x;
	prev_left_stick_y = left_stick_y;
	prev_right_stick_x = right_stick_x;
	prev_right_stick_y = right_stick_y;
	prev_left_trigger = left_trigger;
	prev_right_trigger = right_trigger;
}

int main(void) {
	int rc = EXIT_FAILURE;
	setup_signal_handlers();
	if (libusb_init(&ctx) != 0) {
		goto out;
	}
	if (setup_uinput() != 0) {
		goto out;
	}

	/*
	 * On UI_DEV_CREATE the kernel will create the device node for this
	 * device. We are inserting a pause here so that userspace has time
	 * to detect, initialize the new device, and can start listening to
	 * the event, otherwise it will not notice the event we are about
	 * to send. This pause is only needed in our example code!
	 */
	sleep(1);
	while (!stop_requested) {
		connect_to_device();
		while (!stop_requested && handle == nullptr) {
			printf("No device found. Retrying in 1 second\n");
			sleep(1);
			connect_to_device();
		}
		if (stop_requested) {
			break;
		}
		printf("Connected to device, waiting for input.\n");
		int actual_length;
		while (!stop_requested) {
			unsigned char data[20];
			const int ret = libusb_bulk_transfer(handle, 0x81, data, sizeof(data), &actual_length, 250);
			if (ret == 0 && actual_length > 0 && actual_length == sizeof(data)) {
				emit_data(data);
#ifdef DEBUG
				for (int i = 0; i < actual_length; i++) {
					printf("%02x ", data[i]);
				}
				printf("\n");
#endif
				continue;
			}

			if (ret == LIBUSB_ERROR_TIMEOUT) {
				// No data received within timeout, just continue waiting but start over so we check `stop_requested`
				continue;
			}

			if (ret != LIBUSB_ERROR_IO) {
				fprintf(stderr, "Error receiving data: %s\n", libusb_error_name(ret));
				break;
			}
		}
		disconnect_device();

		if (!stop_requested) {
			printf("Connection terminated. Retrying in 1 second.\n");
			sleep(1);
		}
	}

	rc = EXIT_SUCCESS;

out:
	cleanup();
	return rc;
}
