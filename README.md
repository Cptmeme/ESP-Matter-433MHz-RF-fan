# 433 MHz RF Fan Controller

This example turns an ESP32 into a **Matter Fan** device that controls a fan
whose original remote uses a 433 MHz OOK link (the SYN115 transmitter /
SYN480R receiver module pair). The ESP32 impersonates the remote: when a Matter
controller changes the fan speed, the firmware transmits the matching 433 MHz
code word through the SYN115 on the DATA pin. The SYN480R receiver drives a
built-in "sniffer" that lets you learn the codes your original remote sends.

It is adapted from the standard esp-matter light example — the light endpoint is
replaced with a `fan` endpoint (FanControl cluster, Off/Low/Medium/High) and the
LED driver is replaced with the RF driver in [`main/rf_fan.cpp`](main/rf_fan.cpp).

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for more information about building and flashing the firmware.

## 1. Hardware / Wiring

| Module           | Signal            | ESP32 pin (default) |
|:-                |:-                 |:-:                  |
| SYN115 (TX)      | DATA / ASK        | GPIO22 (`RF_TX_GPIO`) |
| SYN480R (RX)     | DATA              | GPIO21 (`RF_RX_GPIO`) |
| both             | VCC / GND         | 3V3 / GND           |

> GPIO22 exists on the classic ESP32 and ESP32-S3. On the ESP32-C3 (whose
> highest GPIO is 21) change `RF_TX_GPIO` in [`main/rf_fan.h`](main/rf_fan.h).
> Add a short (~17 cm) wire antenna to both boards for usable range.

## 2. Learn your remote's codes (one-time)

The fan code table in [`main/rf_fan.cpp`](main/rf_fan.cpp) ships with `0x000000`
placeholders, so the fan will not respond until you fill it in:

1. Build & flash, then open the serial monitor. The sniffer starts automatically.
2. Press a speed button on your original remote near the SYN480R.
3. Note the logged line, e.g. `LEARN: captured 24-bit code = 0x123456 (base ~350 us)`.
4. Put that value in `s_fan_codes[]` (OFF / LOW / MEDIUM / HIGH), and set
   `RF_BIT_BASE_US` in `rf_fan.h` to the reported base time.
5. Re-flash. Changing the Matter fan speed now drives the real fan.

The on-board button cycles Off → Low → Medium → High for quick local testing.

## 3. Behaviour

- **Speed mapping** — Matter `Off/Low/Medium/High` (speed 0-3) drive the four
  remote buttons `Power 1 / Power 2 / Power 3 / 15-min max`. This unit has no
  true "off"; Power 1 is the lowest continuous speed, exposed as Matter's Off.
- **15-min boost auto-off** — selecting High (the "15 min max power" button)
  arms a timer (`FAN_BOOST_MINUTES`, default 15). When it elapses the fan is
  returned to speed 0. Choosing any other speed first cancels the timer.
- **Two-way sync** — the SYN480R receiver listens continuously. When it hears a
  *known* code (i.e. you pressed the physical remote), it updates the Matter
  state to match **without** re-transmitting, so controllers always show the
  real speed. Unknown codes are logged for learning (see step 2).

## 4. Post Commissioning Setup

No additional setup is required. Control the fan via the FanControl cluster
(FanMode or PercentSetting) from any Matter controller.

## 3. Device Performance

### 3.1 Memory usage

The following is the Memory and Flash Usage.

-   `Bootup` == Device just finished booting up. Device is not
    commissionined or connected to wifi yet.
-   `After Commissioning` == Device is connected to wifi and is also
    commissioned and is rebooted.
-   device used: esp32c3_devkit_m
-   tested on:
    [6a244a7](https://github.com/espressif/esp-matter/commit/6a244a7b1e5c70b0aa1bf57254f19718b0755d95)
    (2022-06-16)

|                         | Bootup | After Commissioning |
|:-                       |:-:     |:-:                  |
|**Free Internal Memory** |108KB   |105KB                |

**Flash Usage**: Firmware binary size: 1.26MB

This should give you a good idea about the amount of free memory that is
available for you to run your application's code.

Applications that do not require BLE post commissioning, can disable it using app_ble_disable() once commissioning is complete.
