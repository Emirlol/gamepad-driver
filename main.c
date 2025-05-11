#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <libusb-1.0/libusb.h>
#include <pthread.h>

#define VIRTUAL_DEVICE_VID 0x1234
#define VIRTUAL_DEVICE_PID 0x5678

#define PAD_VID 0x046d
#define PAD_PID 0xc21f

int fd;
libusb_device_handle *handle;
libusb_context *ctx;
pthread_mutex_t lock;

typedef enum {
	DPAD_UP = 01,
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

static void setupAbs(const int type, const int min, const int max, const int res, const int flat) {
	if (ioctl(fd, UI_SET_ABSBIT, type) == -1) {
		perror("Failed to set ABS bit");
		raise(SIGTERM);
	}
	struct uinput_abs_setup abs = {
		.code = type,
		.absinfo = {
			.minimum = min,
			.maximum = max,
			.resolution = res,
			.flat = flat
		}
	};

	if (ioctl(fd, UI_ABS_SETUP, &abs) == -1) raise(SIGTERM);
}

void handle_signal(const int signal) {
	if (handle != nullptr) {
		libusb_release_interface(handle, 0);
		libusb_attach_kernel_driver(handle, 0);
		libusb_close(handle);
		handle = nullptr;
	}
	if (fd > 0) {
		sleep(1); // Give userspace some time to read the events before we destroy the device with UI_DEV_DESTOY.
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
		fd = -1;
	}
	if (ctx != nullptr) libusb_exit(ctx);
	pthread_mutex_destroy(&lock);
	exit(signal);
}

void setup_uinput() {
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("Failed to open /dev/uinput");
		raise(SIGTERM);
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	constexpr int buttons[] = {BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_SOUTH};

	for (int i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, buttons[i]);

	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	setupAbs(ABS_X, -32768, 32767, 0, 0);
	setupAbs(ABS_Y, -32768, 32767, 0, 0);
	setupAbs(ABS_RX, -32768, 32767, 0, 0);
	setupAbs(ABS_RY, -32768, 32767, 0, 0);
	setupAbs(ABS_Z, 0, 255, 0, 0);
	setupAbs(ABS_RZ, 0, 255, 0, 0);
	setupAbs(ABS_HAT0X, -1, 1, 0, 0);
	setupAbs(ABS_HAT0Y, -1, 1, 0, 0);

	struct uinput_setup usetup = {
		.id = {
			.bustype = BUS_USB,
			.vendor = PAD_VID,
			.product = PAD_PID,
		},
		.name = "Input handler"
	};

	ioctl(fd, UI_DEV_SETUP, &usetup);
	ioctl(fd, UI_DEV_CREATE);
}

void connect_to_device() {
	libusb_device **list;
	const ssize_t count = libusb_get_device_list(ctx, &list);
	for (ssize_t i = 0; i < count; i++) {
		libusb_device *device = list[i];
		struct libusb_device_descriptor desc;
		libusb_get_device_descriptor(device, &desc);
		if (desc.idVendor == PAD_VID && desc.idProduct == PAD_PID) {
			libusb_open(device, &handle);
			if (handle != nullptr) {
				if (libusb_kernel_driver_active(handle, 0) == 1) {
					libusb_detach_kernel_driver(handle, 0);
				}
				libusb_release_interface(handle, 0);
				libusb_claim_interface(handle, 0);
				libusb_ref_device(device);
			}
			libusb_free_device_list(list, 1);
			break;
		}
	}
}

void setup_signal_handlers() {
	struct sigaction sa;
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGQUIT, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	// SIGKILL is not included because it cannot be caught.
}

static int prev_buttons_one = 0, prev_buttons_two = 0;
static int prev_left_stick_x = 0, prev_left_stick_y = 0;
static int prev_right_stick_x = 0, prev_right_stick_y = 0;
static int prev_left_trigger = 0, prev_right_trigger = 0;

void emitBasedOnButtons(const unsigned char data[]) {
	const int buttonsOne = data[2];
	if (buttonsOne != prev_buttons_one) {
		if (buttonsOne & DPAD_DOWN) {
			emit(EV_ABS, ABS_HAT0Y, 1);
		} else if (buttonsOne & DPAD_UP) {
			emit(EV_ABS, ABS_HAT0Y, -1);
		} else {
			emit(EV_ABS, ABS_HAT0Y, 0);
		}

		if (buttonsOne & DPAD_LEFT) {
			emit(EV_ABS, ABS_HAT0X, -1);
		} else if (buttonsOne & DPAD_RIGHT) {
			emit(EV_ABS, ABS_HAT0X, 1);
		} else {
			emit(EV_ABS, ABS_HAT0X, 0);
		}
		emit(EV_KEY, BTN_START, (buttonsOne & START) ? 1 : 0);
		emit(EV_KEY, BTN_BACK, (buttonsOne & BACK) ? 1 : 0);
		emit(EV_KEY, BTN_THUMBL, (buttonsOne & LEFT_STICK) ? 1 : 0);
		emit(EV_KEY, BTN_THUMBR, (buttonsOne & RIGHT_STICK) ? 1 : 0);
	}
	const int buttonsTwo = data[3];
	if (buttonsTwo != prev_buttons_two) {
		emit(EV_KEY, BTN_TL, (buttonsTwo & LB) ? 1 : 0);
		emit(EV_KEY, BTN_TR, (buttonsTwo & RB) ? 1 : 0);
		emit(EV_KEY, BTN_SOUTH, (buttonsTwo & A) ? 1 : 0);
		emit(EV_KEY, BTN_EAST, (buttonsTwo & B) ? 1 : 0);
		emit(EV_KEY, BTN_NORTH, (buttonsTwo & X) ? 1 : 0);
		emit(EV_KEY, BTN_WEST, (buttonsTwo & Y) ? 1 : 0);
	}
	// const int leftStickX = ((data[6] << 8) | data[7]) - 32768;
	const int leftStickY = ((data[8] << 8) | data[9]) - 32768;
	const int rightStickX = ((data[10] << 8) | data[11]) - 32768;
	const int rightStickY = ((data[12] << 8) | data[13]) - 32768;
	// if (leftStickX != prev_left_stick_x) emit(EV_ABS, ABS_X, leftStickX);
	if (leftStickY != prev_left_stick_y) emit(EV_ABS, ABS_Y, leftStickY);
	if (rightStickX != prev_right_stick_x) emit(EV_ABS, ABS_RX, rightStickX);
	if (rightStickY != prev_right_stick_y) emit(EV_ABS, ABS_RY, rightStickY);
	const int leftTrigger = data[4] * 32767 / 255;
	const int rightTrigger = data[5] * 32767 / 255;
	if (leftTrigger != prev_left_trigger || rightTrigger != prev_right_trigger) {
		int difference = rightTrigger - leftTrigger;
		if (abs(difference) <= 6) difference = 0; // Prevent "stick drift"
		emit(EV_ABS, ABS_X, difference);
	}

	emit(EV_SYN, SYN_REPORT, 0);

	prev_buttons_one = buttonsOne;
	prev_buttons_two = buttonsTwo;
	// prev_left_stick_x = leftStickX;
	prev_left_stick_y = leftStickY;
	prev_right_stick_x = rightStickX;
	prev_right_stick_y = rightStickY;
	prev_left_trigger = leftTrigger;
	prev_right_trigger = rightTrigger;
}

int main(void) {
	pthread_mutex_init(&lock, nullptr);
	setup_signal_handlers();
	libusb_init(&ctx);
	setup_uinput();

	/*
	 * On UI_DEV_CREATE the kernel will create the device node for this
	 * device. We are inserting a pause here so that userspace has time
	 * to detect, initialize the new device, and can start listening to
	 * the event, otherwise it will not notice the event we are about
	 * to send. This pause is only needed in our example code!
	 */
	sleep(1);
	while (true) {
		connect_to_device();
		while (handle == nullptr) {
			printf("No device found. Retrying in 1 second\n");
			sleep(1);
			connect_to_device();
		}
		printf("Connected to device, waiting for input.\n");
		int actual_length;
		while (true) {
			unsigned char data[20];
			const int ret = libusb_bulk_transfer(handle, 0x81, data, sizeof(data), &actual_length, 0);
			if (ret == 0 && actual_length > 0 && actual_length == sizeof(data)) {
				emitBasedOnButtons(data);
				// for (int i = 0; i < actual_length; i++) {
				// 	printf("%02x ", data[i]);
				// }
				// printf("\n");
			} else if (ret != LIBUSB_ERROR_TIMEOUT && ret != LIBUSB_ERROR_IO) {
				fprintf(stderr, "Error receiving data: %s\n", libusb_error_name(ret));
				break;
			}
		}
		printf("Connection terminated. Retrying in 1 second.\n");
		sleep(1);
	}
}
