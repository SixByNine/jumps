import subprocess
from ..subcomponent import SubComponent, subcomponentmethod

class TelescopeInterface(SubComponent):


    def __init__(self,backend):
        """
        This is the Telescope Interface subcomponent. It is responsible for reading the telescope status and detecting
        observation start
         """
        super().__init__(looptime=0.2)
        self.backend=backend

    def loop(self):
        super().loop()

    def stop(self):
        super().stop()

    def final(self):
        super().final()


