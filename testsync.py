#!/bin/env python3
import sys, time

print("Test started",file=sys.stderr)

import max3100
import serial
import RPi.GPIO as GPIO

baud = 57600
resetPin = 23
SYNC = (0xAA,0x0D,0x00,0x00,0x00,0x00)
SYNCACK = (0xAA,0x0E,0x0D,0x00,0x00,0x00)
BAUD_56000 = (0xAA,0x07,0x1F,0x01,0x00,0x00)

usemax3100 = True

if usemax3100:
  print("Initialize MAX3100 interface",file=sys.stderr)
  s = max3100.MAX3100(0,0,baud=baud,spispeed=7800000,maxmisses=20)
else:
  print("Initialize serial interface",file=sys.stderr)
  s = serial.Serial("/dev/serial0",baud)
  s.reset_output_buffer()
  s.reset_input_buffer()

def send(s,values):
    print("Write:","%02d"%(len(values),),":"," ".join(map(lambda b: "%02X"%b,values)),file=sys.stderr)
    s.write(values)
    
def recv(s):
    readString = b''
    bytesToRead = s.in_waiting
    while s.in_waiting > 0:
        readString += s.read()
        time.sleep(0.00005)
    if len(readString) > 0:
        print(" Read:","%02d"%(len(readString),),":"," ".join(map(lambda b: "%02X"%b,readString)),file=sys.stderr)
    return readString

# Initialize and reset!
print("Initialize uCAM",file=sys.stderr)

GPIO.setmode(GPIO.BCM)
GPIO.setwarnings(False)
GPIO.setup(resetPin,GPIO.OUT)
GPIO.output(resetPin,GPIO.LOW)
time.sleep(0.1)
GPIO.output(resetPin,GPIO.HIGH)

time.sleep(1)

delay = 0.005    #5ms
delayinc = 0.001 #1ms
nextattempt = 0

good = False
for i in range(60):
    while (time.time() < nextattempt):
        time.sleep(0.00001)
    # print("%.6f"%(time.time()-nextattempt,))
    nextattempt = time.time()+delay
    
    send(s,SYNC)
    # time.sleep(0.001)
    received = recv(s)
    
    # print(received[:3],SYNCACK[:3],received[:3]==SYNCACK[:3])
    if received[:3] == bytes(SYNCACK[:3]):
        print("attempt succeeded",i+1)
        good = True
        break
    delay += delayinc;

if good:
    send(s,SYNCACK)
    time.sleep(0.01)
    send(s,BAUD_56000)
    time.sleep(0.01)
    received = recv(s)
