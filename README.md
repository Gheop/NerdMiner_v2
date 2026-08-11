# NerdMiner v2, fork with verified fixes and a rebuilt SHA path

This firmware turns an ESP32 board into a solo Bitcoin lottery miner. It speaks Stratum,
runs on about 25 boards, and shows its work on a small screen. This fork adds measured
performance work on the hardware SHA engine and fixes for several failures that made a
miner report a healthy rate while producing nothing a pool would accept.

[![CI](https://github.com/Gheop/NerdMiner_v2/actions/workflows/ci.yml/badge.svg?branch=all-fixes)](https://github.com/Gheop/NerdMiner_v2/actions/workflows/ci.yml)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

## Quick start

You need a supported ESP32 board, a USB cable, and a Bitcoin address.

1. Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/).
2. Clone this repository.
3. Find your board in `platformio.ini`. The environment name is in square brackets.
4. Connect the board to your computer.
5. Build and flash the firmware:

   ```bash
   pio run -e NerdminerV2 -t upload
   ```

6. Wait for the board to start.
7. Connect your phone to the WiFi network `NerdMinerAP`. The password is `MineYourCoins`.
8. Open the configuration page. Enter your WiFi details and your Bitcoin address.
9. Save. The board restarts and starts to mine.

Replace `NerdminerV2` with the environment for your board.

## Installation

### From source with PlatformIO

This is the only build path in this repository. The project pins
`platform = espressif32@6.6.0` and uses the Arduino framework.

1. Install PlatformIO Core.
2. Clone this repository.
3. Select your environment from `platformio.ini`.
4. Run the build:

   ```bash
   pio run -e YOUR_ENV
   ```

5. Connect the board with USB.
6. Flash the firmware:

   ```bash
   pio run -e YOUR_ENV -t upload
   ```

Do not run `pio run` without `-e`. The default target builds 35 environments.

### From a prebuilt binary

The `bin/` folder holds binaries for three boards only: DUO, ESP32-devKit, and
LILYGO T-Display S3.

**These binaries date from before this fork. They contain none of the changes described
here.** Build from source if you want the fixes and the performance work.

To flash a prebuilt binary, use the [online ESP tool](https://espressif.github.io/esptool-js/)
with Chrome, Chromium, or Brave. Select each `.bin` file from the folder for your board.

### Flash problems

- Set the speed to 115200 bps.
- If the upload stops, put the board in boot mode. Unplug the cable. Hold the lower right
  button. Plug the cable again. Flash again.
- On ESP32-WROOM boards, hold the boot button, press reset, then flash.
- If the WiFi fails after a flash, erase the whole flash in ESP tool. Then flash again.

## Usage

### Configure the miner with the WiFi portal

1. Power the board.
2. Connect your phone to the access point `NerdMinerAP`. The password is `MineYourCoins`.
3. Open `192.168.4.1` in a browser.
4. Enter your WiFi name and password.
5. Enter your Bitcoin address. Add a worker name after a dot, for example
   `YOUR_BTC_ADDRESS.worker1`.
6. Save the settings. The board restarts.

The portal closes after 180 seconds. The board then restarts and tries the saved network.

### Configure the miner with a file

Boards with an SD card slot read `/config.json` from the card. Boards without one read
the same file from SPIFFS.

**The key names differ between the two.** See the [Configuration](#configuration)
section for both sets.

To write the SPIFFS file, put your `config.json` in the `data/` folder. Then upload it:

```bash
pio run -e YOUR_ENV -t uploadfs
```

### Update a board over WiFi

Two environments support OTA: `NerdminerV2-OTA` and `ESP32-devKitv1-OTA`. Both read the
password from the `NERDMINER_OTA_PWD` variable.

1. Export the password:

   ```bash
   export NERDMINER_OTA_PWD='YOUR_PASSWORD'
   ```

2. Find the board address. The board publishes an mDNS name:

   ```bash
   getent hosts nerdminer-XXXX.local
   ```

   `XXXX` is the last two bytes of the MAC address, in lowercase hexadecimal.

3. Send the firmware:

   ```bash
   pio run -e NerdminerV2-OTA -t upload --upload-port MINER_IP
   ```

4. Send the configuration, if you changed it:

   ```bash
   pio run -e NerdminerV2-OTA -t uploadfs --upload-port MINER_IP
   ```

OTA starts only when `OTA_PASSWORD` is set at build time. Without it, the board mines
normally and prints `OTA disabled: no OTA_PASSWORD set at build time`.

OTA listens on UDP port 3232. A TCP probe such as `nc -z HOST 3232` reports the port
closed even when OTA works. Test with a real upload.

**Check the result. The return code does not prove that the update took effect.** After
an OTA, the active partition is the other slot. A later USB flash at `0x10000` writes the
inactive slot, and the board boots the old firmware with no error.

### Add OTA to another environment

Create an environment that extends yours:

```ini
[env:my-board-OTA]
extends = env:my-board
upload_protocol = espota
build_flags =
    ${env:my-board.build_flags}
    -D OTA_PASSWORD='"${sysenv.NERDMINER_OTA_PWD}"'
```

Keep the password in your environment. Do not write it in the file.

### Update several boards at once

`pio run -t upload` revalidates the whole project on each call. That costs about a minute
per board. Build once, then call `espota.py` for each board in parallel:

```bash
pio run -e NerdminerV2-OTA
for ip in 192.168.1.41 192.168.1.42 192.168.1.43; do
  python ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
     -i "$ip" -p 3232 --auth="$NERDMINER_OTA_PWD" \
     -f .pio/build/NerdminerV2-OTA/firmware.bin &
done
wait
```

Six boards take under thirty seconds this way.

### Read the screens

The firmware shows several screens. Press the button to change screen.

- Mining screen: hashrate, shares, best difficulty, temperature.
- Clock screen: time and mining data.
- Global stats screen: network difficulty and block height.

On one-button boards, a single click changes the screen. A double click rotates it. On
two-button boards, the second button changes the screen.

## What this fork changes

### Hashrate

Each pair below is one board measured before and after, never one board against another.
Runs last five minutes. Every submitted hash is cross-checked against a software
implementation.

| Chip | Upstream `main` | This branch | Gain |
|---|---|---|---|
| ESP32 classic (D0WD-V3 DevKit) | 354.0 kH/s | 754.7 kH/s | +113% |
| ESP32-S3 (T-Display-S3) | 253.6 kH/s | 303.0 kH/s | +19.5% |

![Hashrate comparison](images/hashrate.svg)

The S3 row uses upstream's own settings, so the comparison isolates the SHA path. The
display options and core pinning below take the same firmware to 315.4 kH/s.

The two chips differ because the S3 spends most of a nonce on the APB bus, near 16 cycles
per register access and about 39 accesses per nonce. Little software cost remains there.
On the classic, software overhead dominated.

Where the gains come from on the classic path:

![ESP32 classic step by step](images/classic-steps.svg)

| Change | Gain | Why it works |
|---|---|---|
| Raw DPORT reads | +28% | `DPORT_REG_READ` becomes a function call when the DPORT workaround is on, and the busy-wait paid it on every poll |
| SHA blocks filled in assembly | +9% | gcc materialises an absolute address per register write; one base register with immediate offsets removes that |
| Block 2 written during block 1 | +16% | Its words do not depend on block 1, and the engine latches its message at `START` |
| Nonce loop inside the assembly | +19% | Each asm block clobbers memory, so a return to C forced a reload of everything five times per nonce |
| Per-nonce DPORT interrupt mask dropped | +5.7% | It protected the two-read DPORT sequence, which the raw read no longer uses |

The raw DPORT read assumes that nothing else touches those registers while the engine
lock is held. That holds here. Check it if your board drives a display or I2C from the
second core.

### Fixed failures

Each of these let a miner report a normal hashrate while producing work no pool accepts.

| Failure | Effect | Issue |
|---|---|---|
| The submitted nonce lost its leading zeros | 1 submission in 16 malformed, including a found block | [#750](https://github.com/BitMaker-hub/NerdMiner_v2/issues/750) |
| A coinbase over 255 bytes was truncated without a message | Every share rejected on pools with a larger coinbase | [#809](https://github.com/BitMaker-hub/NerdMiner_v2/issues/809) |
| USB CDC writes blocked the stratum task | 19% of the hashrate lost on S3 and C3 boards plugged into a host | [#810](https://github.com/BitMaker-hub/NerdMiner_v2/issues/810) |
| A pool password over 19 characters overflowed a 20-byte buffer | Memory corruption in `.bss` on each pool connection | [#811](https://github.com/BitMaker-hub/NerdMiner_v2/issues/811) |
| `checkValid()` compared against stack garbage | The function could loop forever | [#797](https://github.com/BitMaker-hub/NerdMiner_v2/issues/797) |
| The stats API queried public-pool.io whatever the configured pool | No worker and no best difficulty on 22 of 24 boards | [#710](https://github.com/BitMaker-hub/NerdMiner_v2/issues/710) |
| WiFi never recovered from a lost link | The miner stayed offline until a manual restart | [#583](https://github.com/BitMaker-hub/NerdMiner_v2/issues/583) |

Each fix also lives on its own branch off upstream `main`. See
[the branch list](https://github.com/Gheop/NerdMiner_v2/branches).

### A known limit on ESP32 classic

With the software check on, this path still produces a rare disagreement on ESP32
classic, near one candidate in 100,000. The S3 shows none over 1.96 million checked
candidates.

Re-reading the digest returns the same wrong value, so the registers hold a wrong result
rather than a misread one. No neighbouring nonce reproduces that hash either.

Espressif erratum CPU-3.16 covers the mechanism. Simultaneous access by the two CPUs to
`0x3ff0_0000~0x3ff1_efff` can lose accesses, and `SHA_TEXT` sits at `0x3FF03000`. Two
mitigations were measured and neither works: a `MEMW` barrier before the start command
costs 1.4% of the hashrate, and stopping the software miner on the second core costs
5.3%. The defect costs 0.02 kH/s across a nine-board fleet, so it is documented and left.

## Configuration

### Runtime settings

The board reads `/config.json` from an SD card, or from SPIFFS when no card is present.
The two files use different key names, for compatibility with existing devices.

| Setting | SD card key | SPIFFS key | Default |
|---|---|---|---|
| WiFi network | `SSID` | set through the portal | `NerdMinerAP` |
| WiFi password | `WifiPW` | set through the portal | `MineYourCoins` |
| Pool address | `PoolUrl` | `poolString` | `public-pool.io` |
| Pool port | `PoolPort` | `portNumber` | `3333` |
| Pool password | `PoolPassword` | `poolPassword` | `x` |
| Bitcoin address | `BtcWallet` | `btcString` | `yourBtcAddress` |
| Time zone | `Timezone` | `gmtZone` | `2` |
| Save stats to NVS | `SaveStats` | `saveStatsToNVS` | `false` |
| Invert screen colours | `invertColors` | `invertColors` | `false` |
| Screen brightness | `Brightness` | `Brightness` | `250` |

The time zone accepts fractions, so `5.5`, `5.75`, and `9.5` work.

Add a worker name to the Bitcoin address after a dot, for example
`YOUR_BTC_ADDRESS.worker1`.

### Build flags

The performance flags default to on and need no change in `platformio.ini`. Set one to
`0` to measure it on its own.

| Flag | Default | Effect |
|---|---|---|
| `RACE_SHA_DIRECT_READ` | `1` | Reads the SHA registers directly instead of the protected DPORT sequence |
| `RACE_ASM_FILL` | `1` | Fills the SHA blocks from one base register in assembly |
| `RACE_PREFILL` | `1` | Writes block 2 while the engine hashes block 1 |
| `RACE_ASM_LOOP` | `1` | Runs the whole nonce loop in assembly, classic path |
| `RACE_ASM_LOOP_S3` | `1` on ESP32-S3, `0` elsewhere | Same, S3 path. The assembly names Xtensa registers, so the C3 and the S2 build without it |
| `PIN_HW_MINER_CORE0` | `1` | Pins the hardware miner to core 0, away from Monitor and Stratum |
| `SCREEN_TIMEOUT_S` | `0` | Blanks the panel after this many seconds. Any button wakes it. `120` is worth about 1% of the hashrate |
| `RACE_DRAW_EVERY_S` | `1` | Seconds between full screen redraws. `3` is worth about 0.7% |
| `VALIDATION` | off | Recomputes every candidate in software and counts disagreements |
| `RACE_KAT` | off | Runs a known-answer test at startup. Needs `VALIDATION` |
| `OTA_PASSWORD` | unset | Password for OTA updates. OTA stays off without it |

### Environment variables

| Variable | Read by | Purpose |
|---|---|---|
| `NERDMINER_OTA_PWD` | `NerdminerV2-OTA`, `ESP32-devKitv1-OTA` | Supplies `OTA_PASSWORD` at build time |

## Development

### Build every environment

`platformio.ini` lists 35 environments in `default_envs`. A bare `pio run` builds all of
them, which takes a long time. Name the environment you want:

```bash
pio run -e NerdminerV2
```

### Run the host tests

Four tests in `test/` run on your computer. They need no board:

```bash
./test/run.sh
```

The script compiles each test, runs it, and returns non-zero when one fails. One test
parses pool JSON and needs the ArduinoJson headers. The script finds them under
`.pio/libdeps/`, which PlatformIO fills on the first build. Fetch them without building
a firmware:

```bash
pio pkg install -e ESP32-devKitv1
```

The script skips that one test, with a message, when the headers are absent.

Each test carries its own copy of the code under test, in both the broken and the fixed
form, so it prints the difference instead of only passing. They do not compile against
`src/`, which needs the Arduino framework, so there is no line coverage figure to report
for the firmware.

### Check the SHA path on your own build

Hash block 125552 through the compiled path and compare. The header, the nonce, and the
hash are public, so this needs no pool:

```
header  0100000081cd02ab7e569e8bcd9317e2fe99f2de44d49ab2b8851ba4a308000000000000
        e320b6c2fffc8d750423db8b1eb942ae710e951ed797f7affc8892b0f1fc122b
        c7f5d74df2b9441a42a14695
nonce   0x9546a142
sha256d 1dbd981f e6985776 b644b173 a4d0385d dc1aa2a8 29688d1e 00000000 00000000
```

Read the header from RAM, not from a flash constant. Loads from flash pass through the
cache and are slower, which gives the engine time that the real loop does not have.

Build with `-D VALIDATION=1 -D RACE_KAT=1` to run this test at startup.

### Continuous integration

Three workflows live in `.github/workflows/`.

| Workflow | Trigger | What it does |
|---|---|---|
| `ci.yml` | pushes to `all-fixes`, pull requests | Runs the host tests, then builds one environment per chip family |
| `release.yml` | pushes to `main` | Builds every environment and publishes a release |
| `prerelease.yml` | pushes to `prerelease` | Same, as a prerelease |

`ci.yml` is the badge at the top of this page. It builds `NerdminerV2`, `ESP32-devKitv1`,
`ESP32-C3-devKitmv1`, `ESP32-S2-mini-wemos` and `ESP32-2432S028R`. The first two are the
boards this fork measures on. The other three cover families we own no hardware for: the
C3 is RISC-V and rejects the Xtensa assembly, so a change to the SHA path that forgets a
guard fails here rather than in someone else's build.

The build jobs need no secrets. `OTA_PASSWORD` comes from the environment and an empty
value is valid, because the firmware then refuses to start the OTA service.

### Measure a change

Compare the same board before and after. Comparisons between boards mean little: we
measured a 15x difference in rare hash disagreements between two boards that ran
byte-identical firmware.

## Supported boards

- LILYGO T-Display S3 ([Aliexpress](https://s.click.aliexpress.com/e/_Ddy7739))
- ESP32-WROOM-32, ESP32-Devkit1 ([Aliexpress](https://s.click.aliexpress.com/e/_DCzlUiX))
- LILYGO T-QT pro ([Aliexpress](https://s.click.aliexpress.com/e/_DBQIr43))
- LILYGO T-Display 1.14 ([Aliexpress](https://s.click.aliexpress.com/e/_DEqGvSJ))
- LILYGO T-Display S3 AMOLED ([Aliexpress](https://s.click.aliexpress.com/e/_DmOIK6j))
- LILYGO T-Display S3 AMOLED Touch ([board info](https://www.lilygo.cc/products/t-display-s3-amoled))
- LILYGO T-Dongle S3 ([Aliexpress](https://s.click.aliexpress.com/e/_DmQCPyj))
- LILYGO T-HMI ([Aliexpress](https://s.click.aliexpress.com/e/_oFII4s2))
- ESP32-2432S028R 2.8 inch ([Aliexpress](https://s.click.aliexpress.com/e/_DdXkvLv))
- ESP32-cam ([board info](https://lastminuteengineers.com/getting-started-with-esp32-cam/))
- M5-StampS3 ([Aliexpress](https://s.click.aliexpress.com/e/_DevABY3))
- Wemos Lolin S3 Mini, Wemos Lolin S2 Mini, Weact S3 Mini, Weact ESP32-D0WD-V3
- ESP32-S3 Devkit, ESP32-C3 Devkit, ESP32-C3 Super Mini
- Waveshare ESP32-S3-GEEK
- ESP32-C3 and ESP32-S3 0.42 inch OLED

Aliexpress links are affiliate links, kept from the upstream project.

The performance work in this fork is measured on two of them: LILYGO T-Display S3 and a
bare ESP32-D0WD-V3 DevKit. The changes are not specific to those boards, but the numbers
are.

A 3D printable case is in [`3d_files/`](3d_files/).

## Contributing

Open an issue on this fork. Pull requests are welcome.

If you report a measurement, say which board, how long the run lasted, and how you
checked that the hashes were correct. A hashrate with no correctness check can rise
because the miner stopped doing useful work.

Upstream project: [BitMaker-hub/NerdMiner_v2](https://github.com/BitMaker-hub/NerdMiner_v2).
Original project: [valerio-vaccaro/HAN](https://github.com/valerio-vaccaro/HAN).

## Licence

MIT. See [LICENSE](LICENSE).
