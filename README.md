# espruino-ida

```
esptool.py --chip esp32 --port /dev/cu.usbserial-110 --baud 115200 erase_flash
```


```
esptool.py --chip esp32 --port /dev/cu.usbserial-110 --baud 115200 \
  write_flash -z \
  0x1000   bootloader.bin \
  0x8000   partitions_espruino.bin \
  0x10000  espruino_esp32.bin
```


zephyr build  
```
west build -b nrf52840dk/nrf52840 -d build --pristine always
```


zephyr flash  
```
west flash -r jlink 
```


zephyr monitor  
```
picocom -b 115200 /dev/ttyACM0
```
