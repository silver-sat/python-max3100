#!/bin/env python3
import sys, time

print("Test started",file=sys.stderr)

import max3100
import serial

print("Initialize serial interface",file=sys.stderr)
ser0 = serial.Serial('/dev/serial0',9600)
ser0.reset_output_buffer()
ser0.reset_input_buffer()

print("Initialize MAX3100 interface",file=sys.stderr)
ser1 = max3100.MAX3100(0,0,baud=9600,maxmisses=20)

print("write max3100, read serial",file=sys.stderr)
buffer = 'UVWXYZ012345678ABCUVWXYZ01234567890123456789ABCUVWXYZ01234567890123456789'.encode('utf8')
print(buffer)
ser1.write(buffer)
print(ser0.read(len(buffer)))

print("write serial, read max3100",file=sys.stderr)
buffer = 'ABCUVWXYZ01234567890123456789ABCUVWXYZ01234567890123456789ABCUVWXYZ01234567890123456789'.encode('utf8')
print(buffer)
ser0.write(buffer)
print(bytes(ser1.read()))

print("close",file=sys.stderr)
ser1.close()

print("Test finished",file=sys.stderr)
