# ESP32-S3 Voice Firmware

Firmware for the Waveshare ESP32-S3 Audio Board.

It runs the hardware side of the voice assistant:

```text
local wakeword -> record speech -> upload WAV -> receive WAV response -> speaker playback
```

The ESP32 handles audio I/O, wakeword, recording, LED feedback, Wi-Fi, backend upload, and playback. STT, agent logic, web search, skills, and TTS run on the backend.

## Folder Structure

```text
firmware/esp32-s3/
  CMakeLists.txt
  partitions.csv
  sdkconfig.defaults
  main/
    main.c                  boot and wiring
    Kconfig.projbuild       menuconfig options
    idf_component.yml       ESP-IDF component deps
    input/                  mic, board audio, WakeNet AFE, recorder
    core/                   voice session, Wi-Fi, backend client
    output/                 speaker/output handling, status LED
    shared/                 small helpers, e.g. WAV wrapping
```

## Requirements

```text
ESP-IDF
Waveshare ESP32-S3 Audio Board
USB cable
Wi-Fi network
reachable backend URL
backend bearer token
```

## Configure

```bash
cd firmware/esp32-s3
source /path/to/esp-idf/export.sh
idf.py menuconfig
```

Set:

```text
Voice Client -> Wi-Fi SSID
Voice Client -> Wi-Fi password
Voice Client -> Backend base URL
Voice Client -> Voice API bearer token
Voice Client -> Speaker playback volume
```

These values are written to local `sdkconfig`.

Do not commit `sdkconfig`. It contains local Wi-Fi and backend credentials.

## Build

```bash
cd firmware/esp32-s3
source /path/to/esp-idf/export.sh
idf.py build
```

## Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

This flashes bootloader, partition table, app, and ESP-SR model partition.

## Monitor

Interactive:

```bash
idf.py -p /dev/ttyACM0 monitor
```

Non-interactive:

```bash
espflash monitor --port /dev/ttyACM0 --non-interactive
```

## Test Flow

```text
1. Start backend.
2. Flash firmware.
3. Say "Alexa".
4. LED ring turns on while recording.
5. Stop speaking.
6. LED ring turns off.
7. ESP uploads WAV to backend.
8. Assistant WAV response plays on speaker.
9. Firmware resets and waits for next wakeword.
```

## Runtime Notes

```text
Wakeword runs locally on ESP-SR WakeNet.
Recording uses pre-roll and silence stop.
Backend URL and token come from ESP-IDF config.
Backend/Wi-Fi errors log and recover instead of killing the loop.
Speaker volume is configured through menuconfig.
```
