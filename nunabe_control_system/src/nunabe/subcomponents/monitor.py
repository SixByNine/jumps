import subprocess
import logging
from ..subcomponent import SubComponent, subcomponentmethod
import queue


class Monitor(SubComponent):


    def __init__(self,backend):
        """
        This is the monitor subcomponent. It is responsible for
          * Collating Logged Messages
          * Collate state from subcomponents
          * Checking for hung processes
          * Identify bad state
          * Memory & Load Monitoring
          * Writing the UI JSON output

          This thread should avoid blocking calls where possible.
          If it becomes an issue we can try using asyncio, but for now keep it simple
         """
        super().__init__(looptime=0.2)
        self.backend=backend
        self.log = logging.getLogger("nunabe.monitor")

    def start(self):
        super().start()
        self.log.info("Monitor Process Started")

    def loop(self):
        super().loop()
        self.log.debug("MONITOR LOOP")
        while True:
            try:
                record = self.backend.logqueue.get(block=0)
                ## handle the records for the UI output.
                #print(record) ## << Not this!
            except queue.Empty:
                break


    def stop(self):
        self.log.info("STOP Monitor")
        super().stop()

    def final(self):
        self.log.info("Monitor Process Stopped")
        super().final()

    @subcomponentmethod
    def update_state(self,state):
        self.state = state