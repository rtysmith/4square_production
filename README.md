# 4square

4square is a small desk clock: four 128x64 OLED panels in a 2x2 grid on a
74x74 mm board, driven by an ESP32-C3 SuperMini. Hours top-left, minutes
top-right, weekday and date along the bottom. Behind the panels there is a
DS3231 real-time clock, an SHT31 temperature and humidity sensor, a light
sensor for auto-dimming, three RGB status LEDs, four buttons and a 24LC256
EEPROM that holds the settings. The firmware is written for panel lifetime
first: every pixel that reaches the glass goes through one function that
applies a burn-in shift and a brightness cap, and a host-side prover checks
every screen the firmware can draw before an image is allowed to ship.

This repository is the firmware, a prebuilt demo image, and enough hardware
notes to flash and build it yourself.

## What is in the repo

    firmware/               the firmware source (Arduino sketch + src/)
    firmware/build.sh       builds the demo or full image with arduino-cli
    release/                prebuilt DEMO image, flash.sh, checksums
    tools/layoutcheck/      the layout prover (runs on your PC, not the board)
    tools/ledcheck/         the LED animation prover
    tools/secretsweep/      lists the credential literals the build must not leak
    hardware/HARDWARE.md    board summary and pin map
    LICENSE                 MIT

There are two ways to build the same source:

| build | flag | radio | needs secrets.h | what it is for |
|---|---|---|---|---|
| demo | `-DDEMO_BUILD` | none compiled in | no | showing the clock to people: walkthrough reel, clock, animations, Fahrenheit, bright |
| full | (none) | WiFi + OTA + live market quotes | yes | running it at home on your own network |

The prebuilt image in `release/` is the demo build.

## Quick flash (prebuilt demo image)

You need `esptool` (`pip install esptool`) and a USB-C cable. The SuperMini is
native USB: plug it in and it appears as `/dev/ttyACM0` on Linux or
`/dev/cu.usbmodem*` on macOS.

    git clone https://github.com/partypancake8/4square_production
    cd 4square_production/release
    ./flash.sh

or, by hand, one line:

    esptool --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 4square-demo-merged.bin

`4square-demo-merged.bin` is the whole flash (bootloader + partition table +
app) and goes at `0x0`. If you only want the app slot, `4square-demo.bin` goes
at `0x10000` with `bootloader.bin` at `0x0`, `partitions.bin` at `0x8000` and
`boot_app0.bin` at `0xe000`. `SHA256SUMS` covers all of them.

If esptool cannot find the chip, hold BOOT on the SuperMini while plugging it
in, then run the command again. Unplug and replug after flashing; the board
boots straight into the clock.

The demo image has no WiFi code in it at all, so it never joins a network and
never needs credentials. Time comes from the DS3231; set it over serial if the
clock chip has never been set (see Serial below).

## Buttons

Each button owns a page. Pressing the same button again cycles that page's
variants. There is no menu to get stuck in.

| button | page | press it again |
|---|---|---|
| MODE | clock | next clock style (12: outline, filled, then five faces each in outline and filled) |
| SET | readings: temperature, humidity, network, uptime, light | second set of readings |
| UP | markets: S&P 500, NVDA, AAPL, BTC | second basket: TSLA, MSFT, AMZN, BTC |
| DOWN | animations | first press starts the showreel; second press drops to a shuffled roster of the 48x48 animations |

Long press (600 ms): MODE toggles the status LEDs; SET opens the settings
page. On the settings page SET moves to the next item, UP/DOWN change it,
MODE leaves. It times out back to the clock after 30 s.

Settings items: LIGHT SENS (auto-dim response), TEMP UNIT (C/F, global),
AUTO OFF (a daily screens-off window), SLEEP AT and WAKE AT (its times, in
15 minute steps). The daily auto-off window ships disabled; the overnight
dim (22:30 to 07:00) ships on.

In the demo build the market quotes are baked in and drift on a fixed walk,
temperatures are in Fahrenheit, and brightness is higher than the shipping
default so it reads across a table.

Until SW1-SW4 are fitted, the four buttons can also be pressed over serial
(`B0`-`B3`, `B0L`-`B3L` for a long press).

## Building from source

Install [arduino-cli](https://arduino.github.io/arduino-cli/), then the
ESP32 core and the four libraries the sketch uses:

    arduino-cli config init
    arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
    arduino-cli core update-index
    arduino-cli core install esp32:esp32
    arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306" "Adafruit BusIO" "Adafruit NeoPixel"

The prebuilt image was built with esp32 core 3.3.10, Adafruit GFX 1.12.6,
SSD1306 2.5.17, BusIO 1.17.4, NeoPixel 1.15.5.

Board and partition scheme:

    FQBN:      esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=no_fs

`CDCOnBoot=cdc` routes Serial to the SuperMini's native USB.
`PartitionScheme=no_fs` gives two 2 MB app slots with OTA and no filesystem;
the firmware does not need one because settings live in the external EEPROM.
Changing the partition scheme needs a USB flash; OTA cannot rewrite the
partition table.

Then:

    firmware/build.sh demo                       # demo image, no secrets needed
    firmware/build.sh full                       # full image, needs src/secrets.h
    firmware/build.sh demo --upload /dev/ttyACM0 # build and flash in one go

Output goes to `firmware/out/<variant>/`: `foursquare.ino.bin` (app) and
`foursquare.ino.merged.bin` (whole flash). The script passes
`-ffile-prefix-map` so the binary carries no local paths, and `--clean` so a
cached core cannot undo that.

By hand, the demo flag is one compiler define:

    arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=no_fs \
      --build-property compiler.cpp.extra_flags=-DDEMO_BUILD \
      --output-dir out firmware/foursquare

Leave the define off for the full build. In the Arduino IDE, open
`firmware/foursquare/foursquare.ino`, pick ESP32C3 Dev Module, USB CDC On
Boot: Enabled, Partition Scheme: "No FS", and add `#define DEMO_BUILD` at the
top of the sketch if you want the demo.

Everything the demo flag changes is inside `#ifdef DEMO_BUILD` and is listed
in one place, `firmware/foursquare/src/demo.h`. It is a build flag, not a
fork.

## Configuring WiFi (full build only)

    cp firmware/foursquare/src/secrets.h.example firmware/foursquare/src/secrets.h

and fill in your SSID and password (two networks are tried in turn; use the
same one twice if you have one) and the SHA256 of your OTA password
(`echo -n 'your-password' | sha256sum`). `secrets.h` is gitignored. The
full build refuses to compile without it and tells you what to do; the demo
build does not read it at all.

The credentials are declared `static const` inside `#ifndef DEMO_BUILD`, and
the OTA password is only ever stored as a digest, so a demo image provably
contains no credential and a full image contains only what it needs.
`tools/secretsweep/literals.sh secrets.h` prints the names and values the
built image should be grepped for; the original flash script does exactly
that against the `.bin` and refuses to flash on a hit.

Once the full build is running on your network, OTA updates go to host
`foursquare-revo` with `espota.py -a <your OTA password>`.

## Settings storage

Settings live in U3, a 24LC256 EEPROM at I2C address 0x50, not in the ESP32's
flash. A firmware flash, a partition change or a full chip erase never loses
them. They are stored as a wear-levelled ring of 128 records, each with a
sequence number and CRC; a torn write costs the last change, not the whole
configuration. Everything read back is range-clamped. Serial `F` factory
resets the ring.

## The one structural rule

`firmware/foursquare/src/screens/display.cpp` is the only file that can write
to a panel. The four `Adafruit_SSD1306` objects have internal linkage there.
Every other drawing routine is handed a `GFXcanvas1` in RAM, and
`disp_commit()` blits canvas to panel, applying the burn-in pixel shift
(a bounded walk inside a 6 px margin), the brightness cap and the night
policy on the way. A new clock face cannot forget to be burn-in safe because
it has no route to the glass. Do not "simplify" this by exposing the panels.

The one thing a layout can still get wrong is drawing in the wrong place,
which is what the prover is for.

## The layout prover

    tools/layoutcheck/build.sh            # shipping build's screens
    DEMO=1 tools/layoutcheck/build.sh     # demo build's screens

It compiles the firmware's own `faces.cpp`, `pages.cpp`, `anim.cpp` and
`defaults.cpp` on your PC against the real Adafruit_GFX library (it looks in
`~/Arduino/libraries/Adafruit_GFX_Library`; set `GFX=` to point elsewhere)
and renders every screen the firmware can draw. It fails if any pixel is
claimed by two elements, if any ink falls outside the safe area
`x[6..121] y[6..57]` that the burn-in shift needs, if an animation has a blank
or discontinuous frame, and it warns on crowding and fill factor. Overlap is
detected by having the firmware mark element boundaries with `ELEM("name")`;
on the host that hook snapshots and clears the canvas, so the pairwise AND of
the element bitmaps is the overlap exactly. On the device the hook is null.
It self-tests against planted bugs before it reports, and needs `g++` and
Python with Pillow for the contact sheet. The demo image in `release/` passed
it (241,001 screens) along with `tools/ledcheck/build.sh`, which does the same
job for the LED animations.

## Serial

115200 baud, native USB CDC. `?` report, `P` settings, `T Y M D h m s` set
the clock, `Z` clear the RTC oscillator-stop flag, `S` rescan screens,
`R [n]` rotate, `D [n]` pin contrast, `I` identify, `F` factory reset,
`B0`-`B3` press MODE/SET/UP/DOWN, `B0L`-`B3L` hold them, `M` network probe.
The board does not reset on DTR/RTS, so you will not see the boot banner by
reopening the port; `?` prints the state instead.

## Hardware

See `hardware/HARDWARE.md` for the board summary and pin map. The PCB design
files (KiCad, gerbers, BOM) live in the separate 4square hardware repository.

## License

MIT, see `LICENSE`.
