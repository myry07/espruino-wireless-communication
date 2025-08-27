# espruino-ida

```
esptool.py --chip esp32 --port /dev/cu.usbserial-110 --baud 115200 \
  write_flash -z \
  0x1000   bootloader.bin \
  0x8000   partitions_espruino.bin \
  0x10000  espruino_esp32.bin
```