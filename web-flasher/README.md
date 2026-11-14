# CrowPanel C64 — Web Flasher

Browser installer for a **new** CrowPanel Advanced 10.1" (ESP32-P4). Uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) + Web Serial (Chrome / Edge).

## Live page (GitHub Pages)

After Pages is enabled for this repo:

**https://dbvandy.github.io/ESP32P4-C64/web-flasher/**

Or open `index.html` from any HTTPS host that serves this folder.

## What it flashes

Merged factory image `CrowPanel_C64-merged-v1.1.4.bin` at offset `0x0`:

| Region | Offset |
|--------|--------|
| Bootloader | `0x2000` |
| Partition table | `0x8000` |
| `boot_app0` | `0xe000` |
| App (v1.1.4) | `0x10000` |

## Rebuild the merged bin

From the repo (with esptool on PATH or Arduino ESP32 package):

```bat
esptool --chip esp32p4 merge-bin -o web-flasher/CrowPanel_C64-merged-v1.1.4.bin ^
  --flash-mode dio --flash-freq 80m --flash-size 16MB ^
  0x2000 rom/CrowPanel_C64.bootloader.bin ^
  0x8000 rom/CrowPanel_C64.partitions.bin ^
  0xe000 rom/boot_app0.bin ^
  0x10000 rom/CrowPanel_C64-v1.1.4.bin
```

Update `manifest.json` `version` / filename when cutting a new release.
