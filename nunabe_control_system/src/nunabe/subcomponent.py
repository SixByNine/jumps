import threading
import queue
from . import nunabe



def subcomponentmethod(f):
    def inner(*args,**kwargs):
        self=args[0]
        self.function_queue.put((f,args,kwargs))
        self.event.set()
    return inner

class SubComponent():

    def __init__(self,looptime=0.5):
        self.looptime=looptime
        self.event = threading.Event()
        self.function_queue = queue.Queue()
        self.run=True
        self.stopping=False
        self.thread=None
        self.lock = threading.RLock()

    def start(self):
        if self.run == False:
            raise nunabe.InternalError("Cannot re-start a stopped subcomponent.")

        self.thread=threading.Thread(target=self.mainthread)
        self.thread.start()

    def mainthread(self):

        while self.run:
            self.execute_queue()
            try:
                self.loop()
            except Exception as e:
                self.handle_exception(e)
            self.event.wait(self.looptime)
            self.event.clear()
        self.final()


    def loop(self):
        pass

    def final(self):
        pass

    def handle_exception(self, e):
        raise e

    @subcomponentmethod
    def stop(self):
        self.run=False
        self.stopping=True
        self.event.set()

    def wait(self):
        self.function_queue.join()

    def join(self):
        #self.stop()
        self.thread.join()

    def execute_queue(self):
        while True:
            try:
                f, args, kwargs = self.function_queue.get(block=False)
                try:
                    f(*args, **kwargs)
                except Exception as e:
                    self.handle_exception(e)
                self.function_queue.task_done()
            except queue.Empty:
                break