# 4square hardware, rev O

Two-layer 74 x 74 mm board, JLCPCB economic PCBA on the bottom side plus a
hand-soldered kit on top. The design files (KiCad, gerbers, BOM, CPL) live in
the separate 4square hardware repository; this file is the summary the
firmware needs.

## What is on it

| ref | part | bus | notes |
|---|---|---|---|
| J5/J6 | ESP32-C3 SuperMini module | | on headers; native USB-C, no UART bridge |
| J1-J4 | 4x SSD1306 128x64 OLED modules, I2C, 0x3C | mux channels | all white panels, no yellow band |
| U1 | PCA9548A I2C mux, 0x70 | main | one OLED per channel; A0-A2 grounded |
| U3 | 24LC256 EEPROM, 0x50 | main | settings store; socketed DIP-8; WP grounded |
| U4 | DS3231MZ RTC, 0x68 | main | CR2032 backup; INT/SQW to GPIO20 (unused by firmware) |
| U5 | SHT31-DIS-B temperature/humidity, 0x44 | main | |
| R7 | phototransistor + R4 1k | ADC | ambient light for auto-dim |
| D2, D4, D5 | 3x GL5050RGB01H-W addressable RGB LEDs | GPIO7 | vertical column, daisy-chained; U6 74AHCT1G125 5 V level shifter on the data line; the chain is driven as 8 LEDs, 3 fitted |
| SW1-SW4 | 6 mm through-hole tactile switches | GPIO | MODE, SET, UP, DOWN; each via a 1k series resistor (R8-R11) to GND, internal pull-ups; not fitted by JLC, solder them yourself |
| F1, D3, D1, J9 | PTC fuse, 5 V TVS, Schottky, rear power jack | | the rear-port power branch; when powering from the SuperMini's USB-C this branch is not in the path |

## Pin map (ESP32-C3, from `firmware/foursquare/src/config.h`)

| GPIO | function |
|---|---|
| 0 | SW1 MODE |
| 1 | SW2 SET |
| 3 | SW3 UP |
| 10 | SW4 DOWN |
| 4 | light sensor, ADC1_CH4 |
| 5 | I2C SDA |
| 6 | I2C SCL |
| 7 | WS2812-style LED data, through U6 |
| 20 | DS3231 INT/SQW (not used by the firmware) |

None of the button pins are C3 strap pins (2, 8, 9), so a stuck button
cannot stop the board booting.

## Panel positions and mux channels

| connector | position | shows | PCA9548A channel |
|---|---|---|---|
| J1 | top-left | hours | 2 |
| J2 | top-right | minutes | 6 |
| J3 | bottom-left | weekday | 1 |
| J4 | bottom-right | date | 7 |

The firmware indexes panels by position (slot 0 = top-left), and the
channel numbers come from the netlist, not from the silkscreen.

## Two things the silkscreen gets wrong on rev O

- The D2/D4/D5 LED footprint chamfer marks pin 1 (+5V) on the silk, but the
  GL5050RGB01H-W's chamfer marks pin 3 (GND). Orient the LEDs by the datasheet
  and a continuity check to ground, not by the silk. Reverse-mounted LEDs
  collapse the 5 V rail.
- R4 reads 10K on the silk. It is 1k.

## I2C

The four OLEDs are behind the mux; the RTC, the sensor and the EEPROM are on
the main bus. 400 kHz. The `Adafruit_SSD1306` constructor's sixth argument
(`clkAfter`) defaults to 100 kHz and silently slows the whole bus; the
firmware passes 400000 for both. A stuck slave can only be freed by clocking
SCL nine times with the pin open-drain, which `bus_recover()` does.
