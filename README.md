Python MAX3100
==============

This project contains a python module for interfacing with serial devices
over SPI from user space using the MAX3100 SPI<->Serial chip via the
spidev linux kernel driver.

Based on the spidev python module (v3.6).

All code is MIT licensed unless explicitly stated otherwise.

Usage
-----

```python
import max3100
serial = max3100.MAX3100(bus,device)
serial.write(bytes)
bytes = serial.read()
```
