import subprocess
from ..subcomponent import SubComponent, subcomponentmethod

class UserInterface(SubComponent):


    def __init__(self,backend):
        """
        This is the UI subcomponent. It is responsible for receiving instructions from users
         """
        super().__init__(looptime=0.2)
        self.backend=backend

    def loop(self):
        super().loop()

    def stop(self):
        super().stop()

    def final(self):
        super().final()


