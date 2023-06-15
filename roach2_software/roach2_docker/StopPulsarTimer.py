#!RoachSpectrometerLauncher

import casperfpga
import sys

def exit_clean():
    try:
        fpga.stop()
    except: pass
    sys.exit()

#ROACH PowerPC Network:
strRoachIP = '192.168.100.2'
roachKATCPPort = 7147

print '\n---------------------------'
print 'Connecting to FPGA...'
fpga = casperfpga.katcp_fpga.KatcpFpga(strRoachIP, roachKATCPPort)

if fpga.is_connected():
    print 'Connected.'
else:
    print 'ERROR connecting to KATCP server.'
    exit_clean()

print 'Deprogramming FPGA...'
fpga.deprogram()
print 'Done.'
