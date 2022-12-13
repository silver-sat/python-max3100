#!/bin/env python3
import sys

print("Test started",file=sys.stderr)

import max3100

ser = max3100.MAX3100()

ser.open(0,0,2,9600)

ser.close()

print("Test finished",file=sys.stderr)
