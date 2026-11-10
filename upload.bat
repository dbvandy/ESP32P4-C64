@echo off
setlocal
set "CLI=C:\Program Files\Arduino CLI\arduino-cli.exe"
set "ESPTOOL=%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe"
set "ROOT=C:\Users\dbvan\ESP32P4 C64"
set "FQBN=esp32:esp32:esp32p4:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled,ChipVariant=prev3"
REM Prefer COM5; fall back to COM6 (CH340 port can move).
set "PORT=COM5"
"%CLI%" board list 2>nul | findstr /I "COM6" >nul && set "PORT=COM6"
"%CLI%" board list 2>nul | findstr /I "COM5" >nul && set "PORT=COM5"
echo Flashing %PORT% (no-reset), then SPI boot via DTR...
"%ESPTOOL%" --chip esp32p4 -p %PORT% --before default-reset --after no-reset write-flash 0x10000 "%ROOT%\build-crowpanel\CrowPanel_C64.ino.bin"
if errorlevel 1 (
  "%CLI%" upload -p %PORT% --fqbn %FQBN% --input-dir "%ROOT%\build-crowpanel" --upload-property "upload.extra_flags=--after=no-reset"
  if errorlevel 1 exit /b %ERRORLEVEL%
)
python "%ROOT%\spi_boot.py" %PORT%
exit /b %ERRORLEVEL%
