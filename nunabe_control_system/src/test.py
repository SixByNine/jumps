#!/usr/bin/env python
import nunabe
import time

be = nunabe.NunaBackend()
be.debug()
be.start()
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

