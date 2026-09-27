# 433 MHz Matter Fan Controller (ESP32-C6, Thread)

A Matter **Fan** device that controls a 433 MHz remote-operated ventilation unit.
The ESP32-C6 impersonates the original remote: Matter speed changes are
transmitted as On-Off-Keying (OOK) codes through a **SYN115** transmitter, and a
**SYN480R** receiver listens so the Matter state stays in sync when the physical
remote is used. Runs over **Matter-over-Thread** (no Wi-Fi).

Adapted from the esp-matter `light` example (the light endpoint was replaced with
a `fan` endpoint and the LED driver with the RF driver).

---

## 1. Hardware & wiring

| Module        | Signal      | ESP32-C6 pin        | Notes |
|:-             |:-           |:-:                  |:-     |
| SYN115 (TX)   | DATA / ASK  | GPIO22 (`RF_TX_GPIO`) | gates the 433 MHz carrier (OOK) |
| SYN480R (RX)  | DATA        | GPIO21 (`RF_RX_GPIO`) | used to learn codes + sync state |
| both          | VCC / GND   | 3V3 / GND           | add a ~17 cm wire antenna to each |
| DevKit button | —           | GPIO9               | on-board BOOT button (cycles speed) |
| DevKit LED    | —           | GPIO8               | on-board RGB LED |

- GPIO22 (TX) and GPIO21 (RX) are free general-purpose pins on the C6.
- Pins are defined in [`main/rf_fan.h`](main/rf_fan.h). Set `RF_RX_GPIO` to
  `GPIO_NUM_NC` to disable the receiver/sniffer.

---

## 2. Behaviour

### Speed mapping
The remote has 4 buttons. There is **no true "off"** — Power 1 is the lowest
continuous ventilation speed, exposed as Matter's `Off`/speed 0.

| Matter FanMode | Speed | Remote button        | enum (`rf_fan_speed_t`) |
|:-              |:-:    |:-                    |:-                       |
| Off            | 0     | Power 1 (lowest)     | `RF_FAN_OFF`            |
| Low            | 1     | Power 2              | `RF_FAN_LOW`            |
| Medium         | 2     | Power 3              | `RF_FAN_MEDIUM`         |
| High           | 3     | 15 min max (boost)   | `RF_FAN_HIGH`           |

FanModeSequence is set to `Off-Low-Med-High` (0). Both `FanMode` and
`PercentSetting` are handled (a percent slider maps 0/≤33/≤66/>66 to the four
speeds), de-bounced so coupled cluster updates don't transmit twice.

### 15-minute boost auto-off
Selecting **High** (the "15 min max power" button) arms a one-shot timer
(`FAN_BOOST_MINUTES`, default 15). When it elapses the fan is returned to speed 0
(Power 1). Choosing any other speed first — via Matter, the percent slider, the
button, or the physical remote — **cancels** the timer, so it will not revert.
Re-selecting boost restarts the countdown.

### Two-way sync (physical remote → Matter)
The SYN480R listens continuously. When it decodes a code matching the table, it
updates the Matter state (`FanMode` + `PercentSetting` + `PercentCurrent`) using
`attribute::report()`, which notifies controllers **without** re-transmitting
(no RF echo, no feedback loop). It also mirrors the boost timer. A held remote
button is de-bounced (acts on a speed change or after ~1.5 s of quiet).
Unknown/noise codes are only logged, never acted on.

### On-board button
Cycles Off → Low → Medium → High → Off for quick local testing.

---

## 3. Code structure

| File | Role |
|:-    |:-    |
| [`main/rf_fan.h`](main/rf_fan.h) | Public RF API, pin/timing config, speed enum |
| [`main/rf_fan.cpp`](main/rf_fan.cpp) | RMT-based OOK transmitter (EV1527/PT2262), receiver task, decoder/sniffer, code table |
| [`main/app_driver.cpp`](main/app_driver.cpp) | Maps FanControl ↔ RF speeds, boost timer, button, RX-sync handler |
| [`main/app_main.cpp`](main/app_main.cpp) | Creates the Matter `fan` endpoint, OpenThread platform config |
| [`main/app_priv.h`](main/app_priv.h) | Fan defaults, `FAN_BOOST_MINUTES`, driver prototypes |

### How it works internally
- **TX**: `rf_fan_set_speed()` is non-blocking — it queues a request to a
  dedicated `rf_tx` task, which builds the OOK frame (EV1527: bit `1` =
  high 3α/low 1α, bit `0` = high 1α/low 3α, then a 31α sync gap) and streams it
  via the RMT peripheral, repeated `RF_TX_REPEATS` (8) times. A burst blocks
  ~350 ms, so it must not run on the Matter/timer threads — hence the queue.
- **RX**: an `rf_sniffer` task uses RMT receive; the decoder estimates the base
  time α, extracts a code word between sync gaps, and either matches it to the
  table (→ sync callback) or logs it (→ learning).
- **Matter**: `attribute::update()` fires the driver callback (transmits);
  `attribute::report()` does not (used for RX sync). Both take the CHIP stack
  lock internally, so they are safe to call from any task.

---

## 4. Configuration knobs

In [`main/rf_fan.h`](main/rf_fan.h):

| Macro | Default | Meaning |
|:-     |:-:      |:-       |
| `RF_TX_GPIO` | `GPIO_NUM_22` | SYN115 data pin |
| `RF_RX_GPIO` | `GPIO_NUM_21` | SYN480R data pin (`GPIO_NUM_NC` to disable) |
| `RF_FAN_ENABLE_SNIFFER` | `1` | build the receiver/sniffer |
| `RF_BIT_BASE_US` | `350` | OOK base time α (µs) — set to the value the sniffer reports |
| `RF_SYNC_LOW_MULT` | `31` | sync gap = α × 31 |
| `RF_TX_REPEATS` | `8` | frames sent per command |
| `RF_MAX_CODE_BITS` | `32` | max code-word length |

In [`main/app_priv.h`](main/app_priv.h): `FAN_BOOST_MINUTES` (default `15`).

---

## 5. Learn your remote's codes (REQUIRED one-time step)

The code table in [`main/rf_fan.cpp`](main/rf_fan.cpp) ships with `0x000000`
placeholders — **the fan will not respond until you fill it in.**

```c
static const rf_command_t s_fan_codes[RF_FAN_SPEED_MAX] = {
    /* RF_FAN_OFF    -> remote "Power 1"  (lowest)        */ { 0x000000, 24 },
    /* RF_FAN_LOW    -> remote "Power 2"                  */ { 0x000000, 24 },
    /* RF_FAN_MEDIUM -> remote "Power 3"                  */ { 0x000000, 24 },
    /* RF_FAN_HIGH   -> remote "15 min max power" (boost) */ { 0x000000, 24 },
};
```

1. Build & flash, open the serial monitor. The sniffer starts automatically.
2. Press each remote button near the SYN480R. Note the logged line, e.g.
   `LEARN: captured 24-bit code = 0x123456 (base ~350 us)`.
3. Put each value in the matching row (`code`, and `nbits` if not 24), and set
   `RF_BIT_BASE_US` in `rf_fan.h` to the reported base time.
4. Re-flash. Matter Off/Low/Med/High now drive the fan, boost auto-reverts, and
   pressing the physical remote keeps Matter in sync.

---

## 6. Build environment (macOS, this machine)

Working toolchain: **ESP-IDF v5.5.4** at `~/.espressif/v5.5.4/esp-idf`,
esp-matter at `~/.espressif/esp-matter`.

A plain `. export.sh` does **not** work in a non-login shell here. Four things
must be set/sourced first (all captured in the build script below):

1. **Python venv** — `export.sh` otherwise picks Homebrew Python 3.14 and looks
   for a venv that doesn't exist. Pin the one the working builds use:
   `export IDF_PYTHON_ENV_PATH=~/.espressif/tools/python/v5.5.4/venv`
2. **Constraints check** — `espidf.constraints.v5.5.txt` is missing but the deps
   are installed, so: `export IDF_PYTHON_CHECK_CONSTRAINTS=no`
3. **`gn` on PATH** — source `esp-matter/export.sh` (adds the CHIP host tools).
4. **`ESP_MATTER_PATH`** — `export ESP_MATTER_PATH=~/.espressif/esp-matter`

> **ccache**: Homebrew `ccache 4.12` was broken (linked against `libfmt.11.dylib`
> while Homebrew had `fmt 12`), which failed every compile. This has since been
> fixed (`brew reinstall ccache`), so `IDF_CCACHE_ENABLE=1` is fine again.

### Working build script
```bash
#!/bin/zsh
set -e
export ESP_MATTER_PATH=~/.espressif/esp-matter
export IDF_PYTHON_ENV_PATH=~/.espressif/tools/python/v5.5.4/venv
export IDF_PYTHON_CHECK_CONSTRAINTS=no
# export IDF_CCACHE_ENABLE=1   # OK now that ccache is fixed
. ~/.espressif/v5.5.4/esp-idf/export.sh
. ~/.espressif/esp-matter/export.sh
cd ~/esp/ESP-Matter-433MHz-RF-fan
idf.py set-target esp32c6   # first time only (or after deleting build/)
idf.py build
```

---

## 7. Project config changes made (vs. the stock light example)

These are permanent edits in the source tree:

1. **`main/app_main.cpp`** — creates a `fan::` endpoint instead of
   `extended_color_light::`; `percent_setting = (uint8_t)0` cast to avoid a
   `nullable<uint8_t>` assignment ambiguity (bare `0` is an ambiguous
   null-pointer constant).
2. **`sdkconfig.defaults`** — `CONFIG_SUPPORT_FAN_CONTROL_CLUSTER=y` (the light
   example ships it as `n`; without it the FanControl cluster server isn't
   compiled and linking fails with undefined `MatterFanControlClusterServer*`
   symbols). esp-matter compiles cluster servers per
   `CONFIG_SUPPORT_*_CLUSTER` via `components/esp_matter/utils/cluster_select/`.
3. **`sdkconfig.defaults.esp32c6`** — Thread overlay (see below).
4. **`main/CMakeLists.txt`** — kept as the stock example (no `REQUIRES`). Adding
   `PRIV_REQUIRES` to `main` **disables** its automatic dependency on all
   components (IDF `project.cmake` only auto-links all components when `main`
   sets no requirements), which broke `nvs_flash.h` / `esp_timer.h`. The RMT and
   timer headers resolve fine through the auto-dependency.

---

## 8. Matter-over-Thread config

Thread is the default for every C6 build via the target overlay
[`sdkconfig.defaults.esp32c6`](sdkconfig.defaults.esp32c6) (auto-applied *after*
`sdkconfig.defaults`, so it wins conflicts while the base still provides the
cluster selection):

```
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_SRP_CLIENT=y
CONFIG_OPENTHREAD_DNS_CLIENT=y
CONFIG_OPENTHREAD_CLI=n
CONFIG_ENABLE_WIFI_STATION=n          # Wi-Fi off; BLE stays on for commissioning
CONFIG_LWIP_IPV6_AUTOCONFIG=n
CONFIG_LWIP_IPV6_NUM_ADDRESSES=8
CONFIG_LWIP_MULTICAST_PING=y
CONFIG_USE_MINIMAL_MDNS=n
CONFIG_ENABLE_EXTENDED_DISCOVERY=y
```

Verified in the built image: `chip_enable_openthread = true`,
`chip_enable_wifi = false`.

> If you ever change the transport or any `CONFIG_SUPPORT_*_CLUSTER`, delete the
> generated `sdkconfig` so it regenerates from the defaults, then rebuild.
> Switching Wi-Fi↔Thread rebuilds the CHIP library (args.gn changes).

---

## 9. Flash, commission, verify

```bash
cd ~/esp/ESP-Matter-433MHz-RF-fan
idf.py -p <PORT> flash monitor
```
(with the same env setup as the build script above.)

- **Commissioning needs a Thread Border Router** (Apple TV/HomePod, Google Nest
  Hub 2nd gen, SmartThings hub, or an ESP Thread border router). Commissioning
  happens over BLE, then the device joins your Thread network. No Wi-Fi.
- After commissioning, control the fan via the FanControl cluster (FanMode or
  PercentSetting) from any Matter controller.

---

## 10. Build status (verified)

- Compiles and links cleanly for **esp32c6** on **IDF 5.5.4** / esp-matter.
- `fan.bin` ≈ **1.67 MB**, ~15% free in the app partition.
- CHIP built for **Thread transport, Wi-Fi disabled**.
- Fan logic, RMT OOK TX/RX, boost timer, and RX-sync all build; runtime RF
  behaviour depends on filling in the learned codes (section 5).

---

## 11. Troubleshooting

| Symptom | Cause / fix |
|:-       |:-           |
| Fan doesn't respond to Matter | Codes not learned — fill `s_fan_codes[]` (section 5) |
| Sniffer logs garbage when idle | Normal SYN480R noise; only matched known codes act. Add antenna, separate the boards |
| Wrong speed / partial codes | Adjust `RF_BIT_BASE_US` to the sniffer's reported base; check `nbits` |
| `nvs_flash.h` / `esp_timer.h` not found | Don't add `REQUIRES`/`PRIV_REQUIRES` to `main/CMakeLists.txt` |
| Undefined `MatterFanControlClusterServer*` at link | `CONFIG_SUPPORT_FAN_CONTROL_CLUSTER=y` missing; delete `sdkconfig`, rebuild |
| `libfmt.11.dylib` not found | Broken Homebrew ccache — `brew reinstall ccache`, or `IDF_CCACHE_ENABLE=0` |
| `gn command not found` | Source `esp-matter/export.sh` |
| Python venv / constraints errors | Set `IDF_PYTHON_ENV_PATH` and `IDF_PYTHON_CHECK_CONSTRAINTS=no` |
