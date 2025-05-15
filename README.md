An application in C which reads the inputs from the device via usb using libusb and sends inputs through a virtual device using uinput, acting as a driver.

This project was only made because I couldn't figure out a proper way to remap analog inputs between each other, as I wanted to bind LT to Lstick x axis- and RT to Lstick x axis+. Currently, this is how the application sends the inputs. The code to allow the left stick to work properly in the x axis is commented out.

This also allows fine-tuning deadzones, as I can see exactly what values are left over after releasing the trigger via `evtest` and set the highest value of that as the deadzone. From my experience LT and RT generally stop at 0, 3 or 6; so it's set to ignore LT & RT <= 6.
With some math, the sensitivity or the active zone of analog inputs (how soon they max out) could be changed as well.

The driver will only work in linux, and only for Logitech F710 as the VID & PID are hardcoded. It might work for other devices if you change that, but the usb reports might be in a different format/have different ranges of values available.
