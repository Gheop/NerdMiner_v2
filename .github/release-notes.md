Prebuilt images of the `all-fixes` branch, for every board in `platformio.ini`.

**Measured:** ESP32 classic 354 → 811 kH/s against upstream `main` on the same board, and ESP32-S3 (T-Display-S3) 313 → 359 kH/s on our boards. Every candidate is re-checked in software during the measurements. Details and method are in the [README](https://github.com/Gheop/NerdMiner_v2/tree/all-fixes#hashrate).

**Only two boards are measured here:** LILYGO T-Display S3 (`NerdminerV2`) and a bare ESP32 DevKit (`ESP32-devKitv1`). The other images build in CI but have not run on hardware on our side. Reports from other boards are welcome in the issues.

## Flash a new board

1. Download `YOUR_ENV_factory.bin` below.
2. Open the [online ESP tool](https://espressif.github.io/esptool-js/) in Chrome, Chromium or Brave.
3. Connect the board, click Connect, set the address to `0x0`, select the file, click Program.
4. Join the `NerdMinerAP` WiFi network (password `MineYourCoins`) and fill in the portal.

`YOUR_ENV_firmware.bin` is the same firmware without bootloader, for OTA updates.

## OTA

These images carry no OTA password, so OTA is off by default. To enable it, set an OTA password of 8 characters or more in the portal, or `otaPassword` in the configuration file. This setting is new in this release. It has been tested through the configuration file on one ESP32 classic; the portal field has not been tested yet.

## Known limit

ESP32 classic boards show rare hash disagreements, about 1 candidate in 100,000. These images do not set the `VALIDATION` build flag, so such a candidate is used as the hardware computed it: at worst a share is rejected by the pool, or a real one is missed. At this rate the effect on the odds is negligible.
