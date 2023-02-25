#!/bin/env python3
import sys, time, math
from collections import defaultdict

print("Test started",file=sys.stderr)

import max3100
import serial
import RPi.GPIO as GPIO

baud = 57600
# baud = 9600
pkgSize = 512
# pkgSize = 128
# pkgSize = 64
resetPin = 23
SYNC = (0xAA,0x0D,0x00,0x00,0x00,0x00)
SYNCACK = (0xAA,0x0E,0x0D,0x00,0x00,0x00)
BAUD = (0xAA,0x07,0x1F,0x01,0x00,0x00) #5760
# BAUD = (0xAA,0x07,0x1F,0x0B,0x00,0x00) #9600
INITIAL_JPEG_3 = (0xAA,0x01,0x00,0x07,0x07,0x07)
PKGSIZE = (0xAA,0x06,0x08,0x00,0x02,0x00) #512
# PKGSIZE = (0xAA,0x06,0x08,0x80,0x00,0x00) #128
# PKGSIZE = (0xAA,0x06,0x08,0x40,0x00,0x00) #64
EXPSETTINGS = (0xAA,0x14,0x02,0x02,0x02,0x00)
SNAPSHOT = (0xAA,0x05,0x00,0x00,0x00,0x00)
GETPICTURE = (0xAA,0x04,0x01,0x00,0x00,0x00)
ACK = (0xAA,0x0E)

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
    print("Write:","%03d"%(len(values),),":"," ".join(map(lambda b: "%02X"%b,values)),file=sys.stderr)
    s.write(values)
    
def recv(s):
    readString = b''
    while s.in_waiting > 0:
        readString += s.read()
        time.sleep(0.00005)
    if len(readString) > 0:
        if len(readString) <= 12:
            print("Read: ","%03d"%(len(readString),),":"," ".join(map(lambda b: "%02X"%b,readString)),file=sys.stderr)
        else:
            print("Read: ","%03d"%(len(readString),),":"," ".join(map(lambda b: "%02X"%b,readString[:12]))+" ...",file=sys.stderr)
    return readString

def ack(s):
    return ACK+(s[1],)
    
def badack(s,r):
    return r[:3] != bytes(ack(s))

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
    if len(received) == 12 and \
        received[:3] == bytes(SYNCACK[:3]) and \
        received[6:8] == bytes(SYNC[:2]):
        print("attempt succeeded",i+1)
        good = True
        break
    delay += delayinc;

if good:
    send(s,SYNCACK)
    
    delay = 0.1
    
    time.sleep(delay)
    send(s,BAUD)
    time.sleep(delay)
    received = recv(s)
    
    if badack(BAUD,received):
       sys.exit(1)
       
    time.sleep(1)
    
    time.sleep(delay)
    send(s,INITIAL_JPEG_3)
    time.sleep(delay)
    received = recv(s)
    
    if badack(INITIAL_JPEG_3,received):
       sys.exit(1)
    
    time.sleep(delay)
    send(s,PKGSIZE)
    time.sleep(delay)
    received = recv(s)
    
    
    if badack(PKGSIZE,received):
       sys.exit(1)
    
    time.sleep(delay)
    send(s,EXPSETTINGS)
    time.sleep(delay)
    received = recv(s)
    
    if badack(EXPSETTINGS,received):
       sys.exit(1)
    
    time.sleep(delay)
    send(s,SNAPSHOT)
    time.sleep(delay)
    received = recv(s)
    
    if badack(SNAPSHOT,received):
       sys.exit(1)
    
    seenpackages = defaultdict(list)
    for attemps in range(5):
    
        time.sleep(2)
        
        send(s,GETPICTURE)
        time.sleep(0.0001)
        camReply = recv(s)
            
        if badack(GETPICTURE,camReply) or len(camReply) != 12:
            sys.exit(1)
        
        imageSize = camReply[-1]*256*256+camReply[-2]*256+camReply[-3]
        print("Picture size:",imageSize,"bytes")

        numPackages = int(math.ceil(imageSize/(pkgSize-6)))
        receivedSize = 0
        receivedPackets = 0
        for indx in range(numPackages):
                    
            if indx < (numPackages-1):
                expectedLen = pkgSize
            else:
                expectedLen = imageSize-(numPackages-1)*(pkgSize-6)+6
            
            ackResponse = ACK+(0,0,indx,0)
            # time.sleep(delay)
            s.clear()
            send(s,ackResponse)
            time.sleep(0.0001)
            pkg = recv(s)
            while len(pkg) == 0:
                pkg = recv(s)

            pkgID0 = pkg[0]
            pkgID1 = pkg[1]
            pkgdatalen = pkg[2]+256*pkg[3]
            pkgverify = pkg[-2]
            verify = (sum(pkg[:-2])&0xff)
                       
            if pkgID0 == (indx+1) and \
                pkgID1 == 0 and \
                pkgverify == verify and \
                len(pkg) == expectedLen and \
                (pkgdatalen+6) == expectedLen:
                print(indx+1,"Good packet!")
                receivedSize += (len(pkg)-6)
                receivedPackets += 1
                seenpackages[indx+1].append((True,pkg))
            else:
                print(indx+1,"Bad packet!")
                seenpackages[indx+1].append((False,pkg))
        
        send(s,ACK+(0,0,0xF0,0xF0)) # done...
        print("Picture size:",imageSize)
        print("received size:",receivedSize)
        print("received packets: %s/%s"%(receivedPackets, numPackages))
        
        bad = False
        for i in seenpackages:
            if not any([t[0] for t in seenpackages[i]]):
                bad = True
                break
        if not bad:
            break
    
    for i in seenpackages:
        if not all([t[0] for t in seenpackages[i]]):
            goodpkg = None
            for good,pkg in seenpackages[i]:
                print(i,len(pkg),
                             "T" if good else "F",
                             " ".join(map(lambda b: "%02X"%b,pkg[:10]))," ... ",
                             " ".join(map(lambda b: "%02X"%b,pkg[-10:])))
                if good:
                    goodpkg = pkg
            if goodpkg:
                p = 0
                for good,pkg in seenpackages[i]:
                    p += 1
                    if good:
                        continue
                    inserts = []
                    ininsert = False
                    k = 0
                    for j in range(len(goodpkg)):
                        if pkg[k:(k+4)] == goodpkg[j:(j+4)]:
                            k += 1
                            if ininsert:
                                ininsert = False
                                inserts.append((insertstart,j-insertstart))
                        elif pkg[k] == goodpkg[j]:
                            k += 1
                        else:
                            if not ininsert:
                               insertstart = j
                               ininsert = True
                    if ininsert:
                        inserts.append((insertstart,len(goodpkg)-insertstart))
                    print(inserts)
                    for ins in inserts:
                        st = max(ins[0]-10,0); ed = min(ins[0]+ins[1]+10,len(goodpkg))
                        for ii in range(st,ed):
                            print("%02X"%goodpkg[ii],end=" ")
                        print()
                        for ii in range(st,ins[0]):
                            print("%02X"%goodpkg[ii],end=" ")
                        for ii in range(ins[1]):
                            print("  ",end=" ")
                        for ii in range(ins[0]+ins[1],ed):
                            print("%02X"%goodpkg[ii],end=" ")
                        print(ins[1])
                        print()
                    
                             
            print()