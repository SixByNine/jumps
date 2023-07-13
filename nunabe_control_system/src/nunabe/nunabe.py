import queue
import logging
import threading
import copy
import uuid
import os

from . import subcomponents
from .subcomponent import SubComponent, subcomponentmethod


class NunaBackend(SubComponent):
    """
    Stores the state of the backend.
    """

    def __init__(self):
        super().__init__(looptime=10)
        self.config = None
        self.state = self.initial_state()
        ## The state lock is used when someone wants to make significant changes to the state of the backend
        self.state_lock = threading.Lock()

        self.logqueue = queue.Queue()
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

        # @todo: make this somehow settable.
        self.digitiser_interface = subcomponents.Roach2(self)

        self.cpu_map = {}

    def initial_state(self):
        return dict(status='Initialising', recording_status='Initialising')

    def start(self):
        super().start()
        self.reload_configurations()
        self.config = self.get_configuration()
        self.monitor.start()
        self.ringbuffer.start()
        self.userinterface.start()
        self.telescopeinterface.start()
        self.digitiser_interface.start()
        self.log.info("Subcomponents Started")
        self.state['recording_status'] = 'Ready'
        self.state['status'] = 'Ready'

    def loop(self):
        # Do stuff!
        pass

    def stop(self):
        try:
            self.log.debug("Stop UserInterface")
            self.userinterface.stop()
        except Exception as e:
            self.log.error(e)
        try:
            self.log.debug("Stop RingBuffer")
            self.ringbuffer.stop()
        except Exception as e:
            self.log.error(e)
        try:
            self.log.debug("Stop TelescopeInterface")
            self.telescopeinterface.stop()
        except Exception as e:
            self.log.error(e)
        try:
            self.log.debug("Stop Digitiser")
            self.digitiser_interface.stop()
        except Exception as e:
            self.log.error(e)
        super().stop()
        self.log.debug("Join Ringbuffer")
        self.ringbuffer.join()
        self.log.debug("Join TelescopeInterface")
        self.telescopeinterface.join()
        self.log.debug("Join UserInterface")
        self.userinterface.join()
        self.log.debug("Join Digitiser")
        self.digitiser_interface.join()

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
        pass  ## to be implemented

    def get_configuration(self, config_name=None):
        ## To be implemented... currently just one hardcoded config whilst testing
        config = {}
        config['dspsr_settings'] = {'run_dspsr': True,
                                    'nbins': 1024,
                                    'nchan': 1600}
        config['telescope_settings'] = dict(
            centre_freq=1532)  # Should this be in config or set by telescope system into state?
        config['system_settings'] = {'ncpu': 16}

        low_ringbuffer = dict(label='low_subband', key='1010', bufsz=838860800, hdrsz=4096, nbufs=10)
        high_ringbuffer = dict(label='high_subband', key='1020', bufsz=838860800, hdrsz=4096, nbufs=10)

        config['ringbuffers'] = [low_ringbuffer, high_ringbuffer]

        config['roach2_settings'] = {
            'low_chans_config': dict(addr='10.0.3.1', port=60000, ctl_fifo='low_chans_control_fifo',
                                     mon_fifo='low_chans_monitor_fifo', interface='ens1f1', priority=-10,
                                     dada=low_ringbuffer),
            'high_chans_config': dict(addr='10.0.3.2', port=60000, ctl_fifo='high_chans_control_fifo',
                                      mon_fifo='high_chans_monitor_fifo', interface='ens1f0', priority=-10,
                                      dada=high_ringbuffer),
            'roach2_reprogram_script':'/opt/roach2_control/reprogram.sh',
            'roach2_udpdb': '/home/mkeith/jumps/roach2_software/roach2_udpdb/roach2_udpdb',
            'interfaces': ['ens1f0', 'ens1f1']
        }
        return config

    @subcomponentmethod
    def start_observation(self, source_name, observing_time):
        """
        This does a lot of things...
         * Check correct modes to run
         * Check for source-specific special cases
         * Check valid state
         * Launch dspsr and any extra things...
        """

        state = self.get_state()  ## Get the current state.

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

        ## Assume for now we reset the cpu map each obesrvation.
        self.cpu_map = {}
        # @todo: Maybe more thought could be added here...
        self.cpu_map = self.digitiser_interface.get_cpu_map(cpu_map=self.cpu_map)

        # Once the CPU map is set, we can actually start the obseving

        # Create the ringbuffers
        for kwargs in self.config['ringbuffers']:
            self.ringbuffer.destroy_buffer(kwargs['label'])
            self.ringbuffer.create_buffer(**kwargs)
        # Wait for the ringbuffers to start.
        self.ringbuffer.execute_queue()

        # Start dspsr
        # @todo: implement this...
        # Wait for dspsr to start...

        # Finally triger the start with the digitiser
        self.digitiser_interface.start_observation(observing_time=observing_time)
        self.digitiser_interface.execute_queue()
        # @todo: Check that we have started?


        pass

    @subcomponentmethod
    def end_observation(self):
        pass

    def debug(self, debug=True):
        if debug:
            self.log.setLevel(logging.DEBUG)
        else:
            self.log.setLevel(logging.INFO)

    class LogHandler(logging.Handler):
        def __init__(self, backend):
            self.backend = backend
            self.logformat = logging.Formatter("{asctime} [{name:<18s}] {levelname}: {message}", style='{')
            super().__init__()

        def emit(self, record):
            print(self.logformat.format(record))
            self.backend.logqueue.put(record)


class InternalError(Exception):
    pass
