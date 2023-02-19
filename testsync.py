#!/bin/env python3
import sys, time

print("Test started",file=sys.stderr)

import max3100
import GPIO
import time

baud = 38400
resetPin = 23
SYNC = bytes(0xAA,0x0D,0x00,0x00,0x00,0x00)
SYNCACK = bytes(0xAA,0x0E,0x0D,0x00,0x00,0x00)

print("Initialize MAX3100 interface",file=sys.stderr)
serial = max3100.MAX3100(0,0,baud=baud)

def send(serial,bytes):
    print("Write:",len(bytes),":"," ".join(map(lambda b: "%02X"%b,bytes)),file=sys.stderr)
    serial.write(bytes)
    
def recv(serial,len=0):
    bytes = serial.read(len=len)
    print(" Read:",len(bytes),":"," ".join(map(lambda b: "%02X"%b,bytes)),file=sys.stderr)

# Initialize and reset!
GPIO.setmode(GPIO.BCM)
GPIO.setwarnings(False)
GPIO.setup(resetPin,GPIO.OUT)
GPIO.output(resetPin,GPIO.LOW)
time.sleep(0.1)
GPIO.output(self.resetPin,GPIO.HIGH)

delay = 0.005    #5ms
delayinc = 0.001 #1ms
respwait = 0.003 #1ms
nextattempt = 0

start = time.time()
for i in range(60):
    while (time.time() < nextattempt):
        time.sleep(0.0002)
    nextattempt = time.time()+delay
    print(time.time()-start)
    serial.clear()
    send(serial,SYNC)
    giveup = time.time()+respwait
    while time.time() < giveup and serial.available() < 6:
        time.sleep(0.0002)
    received = recv(serial,6)    
    if received[:3] == SYNCACK[:3]:
        break
    delay += delayinc;

giveup = time.time()+respwait
while time.time() < giveup and serial.available() < 6:
    time.sleep(0.0002)
received = recv(serial,-6)
send(serial,SYNCACK)

