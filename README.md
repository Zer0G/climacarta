# ClimaCarta

ESP-IDF firmware for a low-power environmental sensor supporting the Waveshare
ESP32-C6-ePaper-1.54 and ESP32-S3-ePaper-1.54G boards from one codebase.

ClimaCarta measures temperature and humidity, stores historical data in flash,
shows readings on an e-paper display, and publishes data over MQTT with
automatic Home Assistant integration.

## Features

- SHTC3 environmental sensor;
- temperature, relative humidity, and dew point;
- configurable sampling through 't_sample' (3600 seconds by default);
- hourly, daily, weekly, monthly, and yearly aggregates;
- daily minimum and maximum temperature;
- CRC-protected circular flash history;
- PCF85063 RTC synchronized through NTP;
- target-specific 200x200 white/black or white/red/yellow/black e-paper display;
- status bar with threshold-colored battery fill and a red Wi-Fi disconnect slash
  on the four-color panel;
- mosquito broker-status icon after a successful MQTT connection and publish;
- environmental data, historical deltas, and charts;
- history page selection with a short BOOT press;
- Wi-Fi and MQTT provisioning through SoftAP, QR code, and captive portal;
- USB Serial/JTAG command console;
- MQTT transmission every 'update_freq * t_sample';
- MQTT command reception;
- Home Assistant MQTT auto-discovery;
- deep sleep with timer and Power-button wake-up;
- temporary active window when connected to a USB host;
- battery percentage calculated from the measured constant-load discharge table,
  interpolated between 2810 mV (0%) and 4160 mV (100%);
- board-specific low-power control for the C6 and S3 variants.

## Supported hardware

- [Waveshare ESP32-C6-ePaper-1.54](https://www.waveshare.com/esp32-c6-epaper-1.54.htm),
  with a white/black display;
- [Waveshare ESP32-S3-ePaper-1.54G](https://www.waveshare.com/esp32-s3-epaper-1.54g.htm),
  with a white/red/yellow/black display.

Both boards include a 1.54-inch e-paper display, SHTC3, PCF85063 RTC,
battery management, and USB-C. They use different MCUs, GPIO maps, display
controllers, and power circuits; the hardware variant is selected at build
time and cannot be changed through provisioning or NVS.

### Pin and device map

| Function | ESP32-C6-ePaper-1.54 | ESP32-S3-ePaper-1.54G |
| --- | --- | --- |
| I2C SDA / SCL | GPIO18 / GPIO8 | GPIO47 / GPIO48 |
| SHTC3 / PCF85063 | 0x70 / 0x51 | 0x70 / 0x51 |
| I/O expander | TCA9554 at 0x20 | not fitted |
| e-paper MOSI / SCLK / CS | GPIO5 / GPIO6 / GPIO7 | GPIO13 / GPIO12 / GPIO11 |
| e-paper DC / BUSY / RESET | GPIO15 / GPIO10 / GPIO11 | GPIO10 / GPIO8 / GPIO9 |
| e-paper power | TCA9554 EXIO0 | GPIO6, active low |
| Battery ADC | ADC1 CH0, GPIO0 | ADC1 CH3, GPIO4 |
| Power button / BOOT | GPIO2 / GPIO9 | GPIO18 / GPIO0 |
| Battery power hold | TCA9554 EXIO5 | GPIO17 |

The C6 board schematic is available in the [doc](doc/) directory. USB host
presence is detected through USB Serial/JTAG on both targets.

## Configuration

ESP-IDF 5.5 or later is required. Use separate build and sdkconfig files so
switching target never reuses incompatible CMake or Kconfig state:

~~~powershell
idf.py --preset c6 build
idf.py --preset s3 build
~~~

Use the same preset with `menuconfig`, `flash`, or `monitor`, for example
`idf.py --preset s3 menuconfig`.

The **RHT sensor configuration** menu provides Wi-Fi credentials, MQTT URI and
credentials, base topic, Home Assistant device ID, NTP server, POSIX timezone,
't_sample', 'update_freq', SHTC3 temperature correction, and NTP
resynchronization interval. Display type is fixed by the selected build target.

The default broker is 'mqtt://192.168.2.99:1883'. Values saved through
provisioning or the console are stored in NVS and take precedence over values
compiled into 'sdkconfig'.

## Smartphone provisioning

1. Hold BOOT for approximately five seconds (GPIO9 on C6, GPIO0 on S3).
2. Connect to the `rht-xxxxxx` SoftAP network.
3. Scan the QR code shown on the e-paper display or open the captive portal.
4. Enter the Wi-Fi SSID and password, MQTT broker, and MQTT credentials.
5. The device stores the configuration in NVS, closes the setup network, and
   then connects to Wi-Fi. The phone switching to another network after the
   confirmation page is expected.

With USB connected, provisioning can also be requested from the serial console
with `provision`. The request is handled after the current display cycle.

The broker field is pre-filled with 'mqtt://192.168.2.99:1883', but it can be
changed through the portal.

## USB console

When a USB host is connected, open the Serial/JTAG port at 115200 baud with
'idf.py monitor'. The console supports:

~~~text
help
show
provision
rtc show
rtc set 2026-10-07 18:30:00
wifi set "Wi-Fi name" "Wi-Fi password"
wifi clear
mqtt set mqtt://192.168.2.99:1883 username password
mqtt clear
reboot
~~~

`rtc set` interprets the supplied date and time in the configured POSIX
timezone. It updates both the ESP32 system clock and the PCF85063, then reads
the hardware RTC back for verification. If the I2C write fails, the system
clock is still updated for the current powered session and the console reports
the RTC error explicitly.

Passwords are never displayed. The 'clear' commands remove values stored in
NVS and allow compiled defaults to be used again.

## Measurement cycle

1. The device wakes from the timer or a wake-up button.
2. It reads the RTC, SHTC3, and battery voltage.
3. It updates the current hourly accumulator.
4. When the hour changes, it stores the completed hourly record in the 'stats' partition.
5. It updates longer-period aggregates.
6. At a periodic slot, or after a manual button wake-up, it connects to Wi-Fi
   and publishes the updated MQTT data.
7. It refreshes the display.
8. It enters deep sleep, except for the console window available while a USB host is connected.

The statistics page plots battery voltage on a fixed vertical scale from 2800
to 4200 mV. The X span grows with the available history: 24 hourly buckets for
the first day, daily buckets up to one month, then two-day buckets up to a
maximum of two months. Its current span in days is shown above the chart's
top-right corner. Empty buckets interrupt the graph instead of joining unrelated
samples. Primary grid lines use a one-pixel-on/one-pixel-off pattern; the
intermediate 100 mV lines use one pixel on and two pixels off. On the four-color
panel both pages use a yellow background, while the plot is black with a red
three-pixel series.

'update_freq=0' and 'update_freq=1' both transmit every sample. With
'update_freq=3', for example, transmission occurs every three samples. Timer
wake-ups are aligned to local midnight: with 't_sample=900', the slots are
00:00, 00:15, 00:30, and so on. A manual button wake-up publishes immediately
but does not move the following periodic slot.

A short Power-button press during the active window forces deep sleep. Pressing
Power again wakes the device and turns the green activity LED back on. On the
ESP32-S3 board, a short BOOT press also wakes the device, changes the history
page and refreshes the display; holding BOOT for approximately five seconds
starts provisioning. On the ESP32-C6 board, BOOT changes and refreshes the page
while the device is already awake.

## MQTT and Home Assistant

With the default configuration:

~~~text
device id: climacarta_<12 hexadecimal digits from the Wi-Fi MAC>
state:     climacarta/<device_id>/state
commands:  climacarta/<device_id>/command
discovery: homeassistant/<component>/<device_id>/<entity>/config
~~~

The device id is generated at runtime from the complete Wi-Fi station MAC, so
multiple boards flashed with the same firmware never share discovery, state,
command, or MQTT client identifiers. Sensor, number, and firmware-update
entities use their corresponding Home Assistant MQTT component types.

Every device also subscribes to the retained broadcast topic:

~~~text
climacarta/broadcast/command
~~~

An OTA broadcast must include a version newer than the installed firmware:

~~~json
{"command":"ota_install","version":"0.1.15"}
~~~

The retained request can remain on the broker: devices that already run the
requested version (or a newer one) ignore it without contacting GitHub. Each
device acknowledges an accepted request and later reports its running version
on `climacarta/<device_id>/ota/status`. An individual OTA command published by
the Home Assistant update entity is retained and cleared by the device after
it has been accepted; the shared broadcast is never cleared by a device.

The retained QoS 1 state JSON contains temperature, humidity, dew point, daily
minimum and maximum, battery percentage and voltage in millivolts, RSSI, plus values and
deltas for the previous hour, the same hour on the previous day, and the
previous week, month, and year.

Discovery messages are published after the first successful MQTT connection,
after new provisioning, and at least every 24 hours thereafter. Historical
periods that are not available yet are published as 'null'.

Firmware up to v0.1.13 used the shared `homeassistant/sensor/rht_epaper`
namespace. After all boards have been upgraded, remove the retained messages
below that legacy namespace with an MQTT administration client and delete any
stale legacy device from Home Assistant. MQTT does not support deleting a
whole retained subtree with a wildcard publish, so each retained `config` and
state topic must be cleared or removed with a broker UI such as MQTT Explorer.

## Build and flash

Use the target-specific commands from the Configuration section. For example,
flash the C6 build with `idf.py --preset c6 -p COMx flash monitor`, or the S3
build with `idf.py --preset s3 -p COMx flash monitor`.

### Visual Studio Code tasks

The workspace provides target-safe tasks under **Terminal > Run Task**. Select
one of `ClimaCarta: Build`, `ClimaCarta: Menuconfig`, or `ClimaCarta: Flash +
Monitor`, then choose the `c6` or `s3` target from the VS Code picker. The flash
task also asks for the serial port and defaults to `COM19`. These tasks invoke
the selected CMake preset directly, so C6 and S3 keep separate build and
sdkconfig files.

If the board contains firmware with an incompatible partition table, it may be
necessary to run the following command once:

~~~powershell
idf.py -p COMx erase-flash
~~~

This also erases NVS credentials and the stored history.

## FOTA through GitHub Releases

The firmware supports HTTPS FOTA from public GitHub Releases using two OTA
application slots and rollback protection. Each hardware target uses a distinct
manifest and binary asset:

| Target | Manifest | Binary |
| --- | --- | --- |
| ESP32-C6 | `manifest-esp32c6-epaper-1.54.json` | `climacarta-esp32c6.bin` |
| ESP32-S3 | `manifest-esp32s3-epaper-1.54g.json` | `climacarta-esp32s3.bin` |

Each manifest includes a mandatory board identifier in addition to the version,
URL, and SHA-256 digest. Firmware rejects a manifest for the other board before
starting the download. See the two examples in `ota/`:

~~~json
{
  "board": "waveshare-esp32-c6-epaper-1.54",
  "version": "1.0.0",
  "url": "https://github.com/zer0g/climacarta/releases/download/v1.0.0/climacarta-esp32c6.bin",
  "sha256": "64_lowercase_or_uppercase_hex_characters"
}
~~~

After building both targets, generate the four target-specific release assets with:

~~~powershell
python tools/package_ota_release.py 1.0.0
~~~

The packager also creates the legacy ESP32-C6 aliases `manifest.json` and
`rht_sensor.bin`. Keep these two files in the release so firmware installed
before the multi-board split can migrate through OTA.

Upload the contents of `release/` to the matching `v1.0.0` GitHub Release. The
device verifies the board ID, manifest, image transport, and SHA-256 digest,
switches slots, and confirms the new image after reboot.

Publish `ota_install` (or `{"command":"ota_install"}`) to the configured MQTT
command topic to request an update. `ota_check` is accepted for forward
compatibility and does not install an image.

An OTA check can also be started locally without MQTT: wake the device and keep
the POWER button pressed for 10 seconds. The display is cleared and shows the
OTA status and progress bar. On the monochrome C6 panel the progress bar uses
partial updates without a full-screen flash. The four-color S3 panel does not
support that update mode, so it keeps the initial OTA screen stable during the
download and refreshes it only when the operation completes or fails.

The first firmware using the dual-slot layout requires a full erase before
flashing because the partition table changes. This erases measurements and
provisioning data; configure Wi-Fi and MQTT again afterwards.

~~~text
Home Assistant --MQTT--> ESP32-C6/S3 --HTTPS--> target-specific GitHub asset
~~~

## License

Copyright (C) 2026 Zer0G

This project is licensed under the GNU General Public License, version 3 or
later (GPL-3.0-or-later). See [LICENSE](LICENSE) for the full text.
