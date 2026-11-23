# WearAware

WearAware is an open-source wearable air-quality monitoring device designed to provide compact, low-power environmental sensing in a smartwatch-sized form factor.

The device is based on the ESP32-S3 and integrates multiple environmental sensors to measure particulate matter, CO$_2$, temperature, humidity, pressure, and related environmental parameters. Measurements can be transmitted wirelessly using Bluetooth Low Energy (BLE) and displayed locally on the integrated e-paper display.

![WearAware hardware in perspective](Size_Comparison_WearAware.png)

## Hardware overview

WearAware combines the following main components:

- ESP32-S3 microcontroller
- Bosch BMV080 particulate matter sensor
- Bosch BME690 environmental sensor
- Sensirion STCC4 CO$_2$ sensor
- Sensirion SHT45 temperature and humidity sensor
- RTC
- Battery fuel gauge
- E-paper display
- USB-C interface
- Physical control buttons

The main PCB components and their locations are shown below.

![WearAware PCB overview](WearAware_Legend.png)

## Firmware

The firmware is developed using [PlatformIO](https://platformio.org/) and contains all files required to compile, upload, and run the current WearAware firmware.

The current implementation focuses on keeping the code simple and easy to understand, while providing the core functionality required to operate the hardware. This includes:

- Sensor initialization and data acquisition
- Power management and sensor duty cycling
- Bluetooth Low Energy communication
- Battery monitoring
- E-paper display control
- Low-power operation using the ESP32-S3 power-management features

The firmware is intended to provide a clear starting point for understanding the hardware and for developing more advanced applications based on the WearAware platform.

## Getting started

1. Install [Visual Studio Code](https://code.visualstudio.com/).
2. Install the PlatformIO extension.
3. Clone this repository.
4. Open the project directory in PlatformIO.
5. Download the **Bosch BMV080 SDK v11.2.0** from
   https://www.bosch-sensortec.com/en/software-tools/software/previous-sdk-bmv-080-versions/
   and accept Bosch's license terms. Build the project once so PlatformIO downloads the SparkFun library
   (the build will fail at this point), then copy these SDK files for the ESP32-S3 into
   `.pio/libdeps/esp32s3/SparkFun BMV080 Arduino Library/src/esp32s3/`:
   `lib_bmv080.a`, `lib_postProcessor.a`, `bmv080.h`, `bmv080_defs.h`
   (see the SparkFun library README for the exact source folder inside the SDK).
6. Connect the WearAware device through USB-C.
7. Build and upload the firmware.

Further documentation on firmware structure, BLE characteristics, hardware setup, and operation will be added as the project develops.

## Repository structure

The repository contains the PlatformIO project files required to build and program the WearAware device, together with the corresponding source code and configuration files.

## License

## License

- **Firmware** (author-written source code and build configuration): MIT License, see [`LICENSE`](LICENSE).
- **Hardware design files**: CERN Open Hardware Licence v2 – Permissive (CERN-OHL-P-2.0).
- **Third-party dependencies** keep their own licenses, see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
- **Bosch BMV080 SDK** is proprietary and is **not** included in this repository. It must be downloaded from Bosch Sensortec by each user, who must accept Bosch's license terms.
