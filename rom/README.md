# CrowPanel C64 firmware (v1.1.4)

Prebuilt ESP32-P4 binaries for CrowPanel Advanced 10.1".

## Files

| File | Flash offset | Notes |
|------|--------------|-------|
| `CrowPanel_C64-v1.1.4.bin` / `CrowPanel_C64.bin` | `0x10000` | App image (usual update) |
| `CrowPanel_C64.bootloader.bin` | `0x2000` | Bootloader |
| `CrowPanel_C64.partitions.bin` | `0x8000` | Partition table |
| `boot_app0.bin` | `0xe000` | ESP boot stub |

Board: 16 MB flash, `app3M_fat9M_16MB`, PSRAM enabled, ChipVariant prev3.

## App-only update (typical)

```bat
esptool --chip esp32p4 -p COMx write-flash 0x10000 CrowPanel_C64-v1.1.4.bin
```

Or from the repo root: `upload.bat` (uses the local build output).

## Full flash (first install / recover)

```bat
esptool --chip esp32p4 -p COMx --flash-mode dio --flash-freq 80m --flash-size 16MB write-flash ^
  0x2000 CrowPanel_C64.bootloader.bin ^
  0x8000 CrowPanel_C64.partitions.bin ^
  0xe000 boot_app0.bin ^
  0x10000 CrowPanel_C64-v1.1.4.bin
```

SHA256 (`CrowPanel_C64-v1.1.4.bin`): `CDE80542861BE453845C6948008E944BA900A5E65B81F044AC1F98E63B00EB31`

## Web flasher (new panel)

Browser install: [web-flasher/](../web-flasher/) — https://dbvandy.github.io/ESP32P4-C64/web-flasher/
