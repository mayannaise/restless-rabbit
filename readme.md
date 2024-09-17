# restless-rabbit

## About

Android lockscreen PIN cracking tool which uses an ESP32S3 with USB OTG support.

Utilises:
* TinyUSB HID to emulate a keyboard
* SDMMC to read PIN codes from dictionary file on SD card and log results to file

## Presrequsites

Requires ESP-IDF to be installed.

Once installed, setup the tools:

```sh
cd ~/esp/esp-idf
source export.sh
```

## Getting Started

Build project and flash to ESP32S3 using:

```sh
idf.py -p /dev/ttyACM0 build flash monitor
```
