import queue
import logging
import threading
import copy

from . import subcomponents
from .subcomponent import SubComponent, subcomponentmethod

class NunaBackend(SubComponent):
    """
    Stores the state of the backend.
    """

    def __init__(self):
        super().__init__(looptime=10)
        self.state = self.initial_state()
        ## The state lock is used when someone wants to make significant changes to the state of the backend
        self.state_lock = threading.Lock()

        self.logqueue=queue.Queue()
        self.log = logging.getLogger("nunabe")
        self.log.setLevel(logging.INFO)
        self.log.addHandler(NunaBackend.LogHandler(self))
        self.log.info("Initialising")
        self.monitor = None
        self.ringbuffer = None
        self.log_queue = queue.Queue()

        self.configurations = {}

        self.monitor = subcomponents.Monitor(self)
        self.ringbuffer = subcomponents.Ringbuffer(self)
        self.telescopeinterface = subcomponents.TelescopeInterface(self)
        self.userinterface = subcomponents.UserInterface(self)

        self.cpu_map={}


    def initial_state(self):
        return {'status':'Initialising'}


    def start(self):
        super().start()
        self.reload_configurations()
        self.monitor.start()
        self.ringbuffer.start()
        self.userinterface.start()
        self.telescopeinterface.start()
        self.log.info("Subcomponents Started")

    def loop(self):
        # Do stuff!
        pass


    def stop(self):
        self.log.debug("Stop UserInterface")
        self.userinterface.stop()
        self.log.debug("Stop RingBuffer")
        self.ringbuffer.stop()
        self.log.debug("Stop TelescopeInterface")
        self.telescopeinterface.stop()
        super().stop()
        self.log.debug("Join Ringbuffer")
        self.ringbuffer.join()
        self.log.debug("Join TelescopeInterface")
        self.telescopeinterface.join()
        self.log.debug("Join UserInterface")
        self.userinterface.join()

        ## stop the monitor last so the user can see the shutdown state.
        self.log.debug("Stop Monitor")
        self.monitor.stop()
        self.log.debug("Join Monitor")
        self.monitor.join()

    def update_state(self, state):
        self.state_lock.acquire()
        for k in state:
            self.state[k] = state[k]
        self.state_lock.release()

    def get_state(self):
        self.state_lock.acquire()
        stat = copy.deepcopy(self.state)
        self.state_lock.release()
        return stat

    @subcomponentmethod
    def restart_monitor(self):
        """
        Stop the monitor thread that watches the backend state
        """
        self.monitor.stop()
        self.monitor.join()
        self.monitor = subcomponents.Monitor(self)
        self.monitor.start()


    @subcomponentmethod
    def reload_configurations(self):
        pass ## to be implemented


    def get_configuration(self, state):
        ## To be implemented
        config={}
        config['dspsr_settings'] = {'run_dspsr':True,
                                    'nbins': 1024,
                                    'nchan': 1600}
        return config

    @subcomponentmethod
    def start_observation(self, source_name):
        """
        This does a lot of things...
         * Check correct modes to run
         * Check for source-specific special cases
         * Check valid state
         * Launch dspsr and any extra things...
        """

        state = self.get_state() ## Get the current state.

        if state['recording_status'] == "recording":
            ## we are already observing...
            if state['source_name'] == source_name:
                self.log.info("Already started this source ({})".format(source_name))
                # we already started on this source... do nothing
                return
            else:
                self.log.warning("Trying to start a new observation before old one has been stopped...")
                ## we need to stop and change to a new source!
                self.end_observation()
                self.start_observation(source_name)
                return

        config = self.get_configuration(state)



        pass

    @subcomponentmethod
    def end_observation(self):
        pass


    def debug(self,debug=True):
        if debug:
            self.log.setLevel(logging.DEBUG)
        else:
            self.log.setLevel(logging.INFO)



    class LogHandler(logging.Handler):
        def __init__(self,backend):
            self.backend=backend
            self.logformat = logging.Formatter("{asctime} [{name:<18s}] {levelname}: {message}",style='{')
            super().__init__()

        def emit(self, record):
            print(self.logformat.format(record))
            self.backend.logqueue.put(record)


class InternalError(Exception):
    pass
