import subprocess
import logging

from ..subcomponent import SubComponent, subcomponentmethod


class RoachInterface(SubComponent):

    def __init__(self,backend):
        """
        This is the Roach Interface subcomponet. It is responsible with organising data streaming from the Roach into the ringbuffer
         """
        super().__init__(looptime=1.0)
        self.backend=backend

    def loop(self):
        super().loop()
        print("ROACH LOOP")

    def stop(self):
        self.backend.log.info("Stopping ROACH interface")
        super().stop()

    def final(self):
        super().final()
