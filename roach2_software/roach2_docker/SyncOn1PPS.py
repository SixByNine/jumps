#!RoachSpectrometerLauncher

import casperfpga
import sys
import time
import socket as socket
import struct as struct
import os.path
from shutil import copyfile
import stat
import numpy as np

def exit_clean():
    try:
        fpga.stop()
    except: pass
    sys.exit()

##### Variables to be set ###########
gateware = "pulchan_r2_2019_Sep_25_1507.fpg"

#ROACH PowerPC Network:
strRoachIP = '192.168.100.2'
roachKATCPPort = 7147

#TenGbE Network:
strTGbEDestinationIPBandTop = '10.0.3.1'
strTGbEDestinationIPBandBtm = '10.0.3.1'
tGbEDestinationPort = 60000

ADCAttenuation = 10
FFTShift = 10 # Until further notice.
RequantGain = 2
StartChan = 2
TVGEnable = True
UseSelfPPS = True

####################################

# Useful little trick to convert from a human-readable IP addr string to an integer like the ROACH wants.
packedIP = socket.inet_aton(strTGbEDestinationIPBandBtm)
tGbEDestinationIPBtm = struct.unpack("!L", packedIP)[0]
packedIP = socket.inet_aton(strTGbEDestinationIPBandTop)
tGbEDestinationIPTop = struct.unpack("!L", packedIP)[0]

print '\n---------------------------'
print 'Configuration:'
print '---------------------------'
print ' FPGA gateware:			    ', gateware
print ' FFT Shift mask:             ', FFTShift
print ' Requantiser gain:           ', RequantGain
print ' Start from channel:         ', StartChan
print ' Destination 10GbE IP (Top):	', strTGbEDestinationIPBandTop, '( ', tGbEDestinationIPTop, ' )'
print ' Destination 10GbE IP (Btm):	', strTGbEDestinationIPBandBtm, '( ', tGbEDestinationIPBtm, ' )'
print '---------------------------'

print '\n---------------------------'
print 'Connecting to FPGA...'
fpga = casperfpga.katcp_fpga.KatcpFpga(strRoachIP, roachKATCPPort)

if fpga.is_connected():
	print 'Connected.'
else:
        print 'ERROR connecting to KATCP server.'
        exit_clean()

print 'Flashing gateware...'

fpga.get_system_information(gateware)
sys.stdout.flush()
time.sleep(0.2)

print "\n---------------------------"
print "Enabling sync with next PPS..."
if UseSelfPPS:
    print "WARNING: USING SELF-GENERATED 1PPS SIGNAL. IF AN EXTERNAL 1PPS IS AVAILABLE IT WILL BE IGNORED."
fpga.registers.sync_ctrl.write(arm=False, self_pps=UseSelfPPS)
time.sleep(0.1)
fpga.registers.sync_ctrl.write(arm=True, self_pps=UseSelfPPS)
sys.stdout.flush()
time.sleep(1.0)


exit_clean()
