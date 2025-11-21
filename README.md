# Xteink X4 Sample

A simple sample project for the Xteink X4 e-ink device using the GxEPD2 library.

![Sample](images/sample.png)

## Hardware

- **Device**: Xteink X4
- **Board**: ESP32-C3 (QFN32)
- **Display**: [4.26" E-Ink (800×480px, GDEQ0426T82, SSD1677 controller)](https://www.good-display.com/product/457.html)
- **Custom SPI pins**: SCLK=8, MOSI=10, CS=21, DC=4, RST=5, BUSY=6

### Resources

- [Hardware Schematics](https://github.com/sunwoods/Xteink-X4) - XteinkX4 circuit diagrams
- [Display Datasheet](https://www.good-display.com/product/457.html) - 4.26" GDEQ0426T82 specifications

## Features

- Basic GxEPD2 display initialization
- Custom SPI pin configuration
- Button input detection via ADC
- Simple text display examples

## Building

```powershell
# Build
platformio run

# Upload
platformio run -t upload

# Monitor
platformio device monitor
```

## Firmware Backup & Restore

### Backup Original Firmware

Before flashing custom firmware, back up the factory firmware:

```powershell
# Read entire 16MB flash
esptool.py --chip esp32c3 --port COM5 read_flash 0x0 0x1000000 firmware_backup.bin
```
```powershell
# Read only app0 (faster)
esptool.py --chip esp32c3 --port COM5 read_flash 0x10000 0x640000 app0_backup.bin
```


### Restore Original Firmware

To restore the backed-up firmware:

```powershell
# Write back the entire flash
esptool.py --chip esp32c3 --port COM5 write_flash 0x0 firmware_backup.bin
```

```powershell
# Write back only app0 (faster)
esptool.py --chip esp32c3 --port COM5 write_flash 0x10000 app0_backup.bin
```

**Important**: Make sure to use the correct COM port for your device.

## Notes

- This uses `GxEPD2_426_GDEQ0426T82` as the display class for the 4.26" 800x480 display
- Display rotation is set to 3 (270 degrees)
- Partial refresh is used for button presses to improve responsiveness

## Open Tasks

- Better font rendering with grayscale support
- ~~Power on/off~~
- SD card reader
- ~~Read battery percentage~~
- WiFi
- Bluetooth

## Button System

The XteinkX4 uses **resistor ladder networks** connected to two ADC pins for button detection. Each button press produces a unique analog voltage that's read via `analogRead()`.

### Button ADC Values

**GPIO1 (4 buttons)**:
- Back: ~3470
- Confirm: ~2655
- Left: ~1470
- Right: ~3

**GPIO2 (2 buttons)**:
- Volume Up: ~2205
- Volume Down: ~3

**GPIO3 (Power button)**:
- Pressed: ~3
- This example uses a 2-second-long press for sleep and a 1.5-second-long press to wake from sleep

**Battery ADC**:
- GPIO0, raw value ranges up to ~2800 when charging. ~2760 when not charging and full.
- Voltage divider is ~2 (2 x 10K resistors), `CONV_FACTOR=1.6113` for [this library](https://github.com/pangodream/18650CL)
- [See here](https://www.pangodream.es/esp32-getting-battery-charging-level) for details how voltage and charge level are calculated
- `CONV_FACTOR` may need calibration depending on the device, `1.5176` working well on mine

### Implementation Notes

- Use threshold ranges (e.g., `value > 3200 && value < 3700`) to detect button presses
- Add debouncing with edge detection (track last button state) to prevent multiple triggers
- Polling every 50ms works well for responsive input
- The resistor ladder allows multiple buttons on a single ADC pin, saving GPIO pins
