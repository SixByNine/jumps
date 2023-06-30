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
        print("UI LOOP")

    def stop(self):
        print("STOPit")
        super().stop()

    def final(self):
        print("FINAL")
        super().final()


