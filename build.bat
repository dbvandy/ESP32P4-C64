@echo off
setlocal
set "CLI=C:\Program Files\Arduino CLI\arduino-cli.exe"
set "ROOT=C:\Users\dbvan\ESP32P4 C64"
REM UART flash via CH340. USBMode=default (TinyUSB); OTG download already eFuse-disabled.
set "FQBN=esp32:esp32:esp32p4:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled,ChipVariant=prev3"
"%CLI%" compile --library "%ROOT%\T-HMI-C64" --warnings default --fqbn %FQBN% --build-path "%ROOT%\build-crowpanel" "%ROOT%\CrowPanel_C64"
exit /b %ERRORLEVEL%
