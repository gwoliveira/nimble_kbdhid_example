### Bluetooth low energy HID Keyboard+mouse example on ESP32 chip using Apache Mynewt NimBLE stack.

Inspired by the Apache NimBLE peripheral role example and the ESP-IDF HID device example from `examples/bluetooth/bluedroid/ble/ble_hid_device_demo`.

This example creates a GATT server and then starts advertising, waiting to be connected
to a GATT client. It supports several GPIO buttons, that can trigger the image to send any
keyboard scan codes to the client (as well as mouse clicks and moves).
Also, this image switches an LED when it receives a "CAPSLOCK on" event from the GATT client.

It uses the ESP32's Bluetooth controller and the NimBLE stack-based BLE host.

### What is the point of using the NimBLE stack on ESP32 instead of the old Bluedroid stack?
1. First of all - the size of the image. (Note: The following performance data was from the original ESP-IDF v4.1 version and may differ with the current PlatformIO setup.)
  - `ble_hid_device_demo` on Bluedroid has a size of 698208 bytes without GPIO buttons.
  - This example has a size of 586840 bytes, and it has GPIO button support.
When I added GPIO buttons to `ble_hid_device_demo` to gain equal functionality to this demo,
its image size grew to 816416 bytes, which is larger than this demo.
2. The second benefit is the time to start the stack. We are not talking about the speed of the stack in common; I haven't
done any research about it. But IMHO NimBLE should be a bit faster because it is newer and BLE-only.
Here are some logs from these two images on the same ESP32 chip.
  - `ble_hid_device_demo` on Bluedroid started to advertise at 1915 milliseconds after boot.
      `I (1915) HID_DEMO: Advertising started.`
  - This example on NimBLE started to advertise at 1087 milliseconds after boot. (x1.75 faster)
      `I (1087) NimBLEKBD_BLEFUNC: Device Address: fc:f5:c4:0e:24:1e`
      `GAP procedure initiated: advertise; disc_mode=2 adv_channel_map=0 ...`
3. Thirdly, I formed an opinion for myself that it is more convenient to describe services and
characteristics of a BLE device using the NimBLE stack than Bluedroid.

All BLE-specific numbers (such as service and characteristics UUIDs) are taken from
the ESP32 `ble_hid_device_demo`; some data was changed, some data was taken as is (for example, the report map).

Documentation sources used:
1. "Getting Started with Bluetooth Low Energy" by Kevin Townsend, Carles Cufí, Akiba, and Robert Davidson
2. From bluetooth.org:
  a. Bluetooth Core Specification - Core_v5.2.pdf
  b. HID SERVICE SPECIFICATION - HIDS_SPEC_V10.pdf
  c. HID OVER GATT PROFILE SPECIFICATION - HOGP_SPEC_V10.pdf
  d. BLUETOOTH ASSIGNED NUMBERS - bluetooth_app_c8.pdf
  e. DEVICE INFORMATION SERVICE - DIS_SPEC_V11r00.pdf
3. From usb.org
  a. Device Class Definition for Human Interface Devices (HID) - hid1_11.pdf
  b. HID Usage Tables - hut1_12v2.pdf
4. ESP-IDF official documentation
  https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/bluetooth/nimble/index.html
5. Apache NimBLE https://mynewt.apache.org/latest/network/ble_hs/ble_hs.html

Tested with PlatformIO and the Espressif 32 Platform (using ESP-IDF v5.x).

### How to Use (with PlatformIO)

This project is configured to be built using [PlatformIO](https://platformio.org/).

1.  **Install PlatformIO:** Follow the official instructions to install the PlatformIO Core CLI or the PlatformIO IDE for VSCode.
2.  **Clone the repository.**
3.  **Open the project:** Open the cloned folder in VSCode (if you have the PlatformIO extension) or navigate to it in your terminal.

### Configure the Project

To change project settings, including GPIO pins or BLE parameters, use the PlatformIO menuconfig interface:

```bash
platformio run -t menuconfig
```

Navigate to "Example configuration menu" to find project-specific settings, such as the bonding method and the GPIO for the CAPSLOCK indicator LED.

### Build, Upload, and Monitor

Use the following PlatformIO commands:

- **Build:**
  ```bash
  platformio run
  ```
- **Upload:**
  ```bash
  platformio run -t upload
  ```
- **Monitor (Serial Output):**
  ```bash
  platformio run -t monitor
  ```
- **Build, Upload, and Monitor all at once:**
  ```bash
  platformio run -t upload -t monitor
  ```

Dive into the sources to know how to change button GPIOs, send mouse moves and clicks, or other keyboard keys.
Have fun with your new BLE Keyboard!
