#!/usr/bin/env python
import nunabe
import time
import sys

be = nunabe.NunaBackend()


be.start()
be.update_state({'source_name':'B0329+54'})
#be.debug()

be.digitiser_interface.reprogram(band_select=0,dont_actually_program=True)
be.digitiser_interface.execute_queue()

print(be.digitiser_interface.get_cpu_map({}))

be.start_observation("test",1800)

be.join()
sys.exit()

time.sleep(2000)
## Todo... make it possible to just join and wait for it to end.
be.stop()
be.join()

sys.exit()

be.ringbuffer.create_buffer("test",key="8888")
time.sleep(1)

print(be.get_state())

time.sleep(1)
print("STOP!!!")
be.stop()
be.join()

#
# time.sleep(30)
# print("TEST restart monitor")
# be.restart_monitor()
# time.sleep(30)
# print("STOP!!!")
# be.stop()
# be.join()

