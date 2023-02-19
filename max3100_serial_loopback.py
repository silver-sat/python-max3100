#!/bin/env python3

import serial
import sys
import time
import array
import itertools
import hashlib
import random

from max3100 import MAX3100

# MAX3100 Loop-back via Serial Port test...
# Or separate programs on different rPIs....
from multiprocessing import Process

def makebytes(length):
    return "".join(chr(ord('A')+random.randint(0,25)) for i in range(0,length)).encode('utf8')

def max3100_thread(baud,length):
    max3100 = MAX3100(0,0,baud=baud)
    while True:
        start = time.time()
        try:
            b = bytes(max3100.read(length))
        except ValueError:
            b = ""
        if len(b) > 0:
            break
    prstr = b.decode('utf8')
    if len(prstr) > 10:
        prstr = prstr[:4]+".."+prstr[-4:]
    print("%4d characters (%s,%s) received (SPI) in %f seconds"%(len(b),prstr,hashlib.md5(b).hexdigest().lower(),time.time()-start))

    time.sleep(2)

    print("%4d characters (%s,%s) to send (SPI)"%(len(b),prstr,hashlib.md5(b).hexdigest().lower()))
    max3100.write(b)

    time.sleep(2)

    while True:
        b = makebytes(6)
        print("%4d characters (%s) to send (SPI)"%(len(b),b))
        max3100.write(b)
        time.sleep(0.0005)
        b = max3100.read()
        print("%4d characters (%s) received (SPI)"%(len(b),bytes(b)))
        time.sleep(1)


def serial_thread(baud,length,delay):
    time.sleep(delay)
    ser = serial.Serial('/dev/serial0',baud)
    ser.reset_output_buffer()
    ser.reset_input_buffer()
    str = makebytes(length).decode('utf8')
    prstr = str
    if len(prstr) > 10:
        prstr = str[:4]+".."+str[-4:]
    print("%4d characters (%s,%s) to send (serial,%d baud)"%(len(str),prstr,hashlib.md5(str.encode('utf8')).hexdigest().lower(),baud))
    ser.write(str.encode('utf8'))
    time.sleep(0.1)
    while ser.in_waiting == 0:
        pass
    start = time.time()
    str = ser.read(length).decode('utf8')
    prstr = str
    if len(prstr) > 10:
        prstr = str[:4]+".."+str[-4:]
    print("%4d characters (%s,%s) received (serial) in %f seconds"%(len(str),prstr,hashlib.md5(str.encode('utf8')).hexdigest().lower(),time.time()-start))
    time.sleep(1)
    while True:
        b = ser.read(6)
        print("%4d characters (%s) to echo (Serial)"%(len(b),b))
        ser.write(b)

    ser.close()
    time.sleep(1)

serial_baud = 38400
# serial_baud = 19200
# serial_baud = 9600
# length = 512
# length = 256
length = 512

if sys.argv[1] == "serial":
    serial_thread(serial_baud,length,5)
elif sys.argv[1] == "max3100":
    max3100_thread(serial_baud)
elif sys.argv[1] == "both":
    p1 = Process(target=serial_thread,args=(serial_baud,length,1))
    p1.start()

    p2 = Process(target=max3100_thread,args=(serial_baud,length))
    p2.start()

    p1.join()
    p2.join()
else:
    print("Please indicate: \"serial\", \"max3100\", or \"both\".")
