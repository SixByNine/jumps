import queue
import logging
import threading
import copy
import signal
import time

from . import subcomponents
from .subcomponent import SubComponent, subcomponentmethod


class NunaBackend(SubComponent):
    """
    Stores the state of the backend.
    """

    def __init__(self):
        super().__init__(looptime=1)
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
        self.dspsr = subcomponents.Dspsr(self)

        # @todo: make this somehow settable.
        self.digitiser_interface = subcomponents.Roach2(self)

        self.cpu_map = {}

    def initial_state(self):
        return dict(status='Initialising', observation_status='Initialising', source_name='Unknown')

    def start(self):
        super().start()

        def signal_handler(signum, frame):
            self.log.warning("Caught SIGINT... stoping urgently")
            signal.signal(signal.SIGINT, signal.default_int_handler)
            self.stop()

        signal.signal(signal.SIGINT, signal_handler)
        self.reload_configurations()
        self.config = self.get_configuration()
        self.monitor.start()
        self.ringbuffer.start()
        self.userinterface.start()
        self.telescopeinterface.start()
        self.dspsr.start()
        self.digitiser_interface.start()
        self.log.info("Subcomponents Started")
        self.state['status'] = 'Ready'

    def loop(self):
        # Check current state
        self.check_observing_state()
        state = self.get_state()
        if state['observation_status'] == 'Completed':
            self.digitiser_interface.cleanup_observation()
            self.dspsr.cleanup_observation()

    def stop(self):

        self.update_state({'observation_status': 'Shutdown', 'status': 'Stopping'})
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
        try:
            self.log.debug("Stop DSPSR")
            self.dspsr.stop()
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

        self.dspsr.join()

        self.update_state({'observation_status': 'Shutdown', 'status': 'Shutdown'})
        ## stop the monitor last so the user can see the shutdown state.
        self.log.debug("Stop Monitor")
        self.monitor.stop()
        self.log.debug("Join Monitor")
        self.monitor.join()

    @subcomponentmethod
    def shutdown(self):
        self.stop()

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
        # @todo: replace this config stuff with a dataclass or something like that.
        ## To be implemented... currently just one hardcoded config whilst testing
        config = {}

        config['telescope_settings'] = dict(
            centre_freq=1532)  # Should this be in config or set by telescope system into state?
        config['system_settings'] = {'ncpu': 16}

        low_ringbuffer = dict(label='low_subband', key='1234', bufsz=838860800, hdrsz=4096, nbufs=20)
        high_ringbuffer = dict(label='high_subband', key='2234', bufsz=838860800, hdrsz=4096, nbufs=20)

        config['ringbuffers'] = [low_ringbuffer, high_ringbuffer]

        config['roach2_settings'] = {
            'low_chans_config': dict(addr='10.0.3.1', port=60000, ctl_fifo='low_chans_control_fifo',
                                     mon_fifo='low_chans_monitor_fifo', interface='ens1f1', priority=-10,
                                     dada=low_ringbuffer, extra_cmd_options=['-F']),
            'high_chans_config': dict(addr='10.0.3.2', port=60000, ctl_fifo='high_chans_control_fifo',
                                      mon_fifo='high_chans_monitor_fifo', interface='ens1f0', priority=-10,
                                      dada=high_ringbuffer, extra_cmd_options=['-F']),
            'roach2_reprogram_script': '/opt/roach2_control/reprogram.sh',
            'roach2_1pps_sync_script': '/opt/roach2_control/sync_1pps.sh',
            'roach2_network_init_script': '/opt/roach2_control/set_network_params.sh',
            'roach2_udpdb': '/home/mkeith/jumps/roach2_software/roach2_udpdb/roach2_udpdb',
            'interfaces': ['ens1f0', 'ens1f1']
        }

        skz_options = "-skz -skzn 5 -skzm 256 -overlap -skz_start 70 -skz_end 490 -skzs 4 -skz_no_fscr -skz_no_tscr".split()
        dspsr_options = "-fft-bench -x 8192 -minram 8192".split()
        dspsr_dict = dict(dspsr='/opt/psr_dev/bin/dspsr', nbins=1024, nchan=256, subint_seconds=10, cuda=None,
                          threads=None,
                          options=dspsr_options, skz_options=skz_options, dada=None, data_root=None, priority=-10)
        config['dspsr'] = {'low_chans': dspsr_dict.copy(), 'high_chans': dspsr_dict.copy()}
        config['dspsr']['low_chans']['dada'] = low_ringbuffer
        config['dspsr']['low_chans']['cuda'] = 1
        config['dspsr']['high_chans']['dada'] = high_ringbuffer
        config['dspsr']['high_chans']['cuda'] = 0
        config['dspsr']['low_chans']['data_root'] = '/mnt/data4/capture_tests/'
        config['dspsr']['high_chans']['data_root'] = '/mnt/data1/capture_tests/'

        self.update_state({'config': config})
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

        self.check_observing_state()
        state = self.get_state()  ## Get the current state.

        if state['observation_status'] != "Ready":
            if state['observation_status'] == "Observing":
                ## we are already observing...
                if state['source_name'] == source_name:
                    self.log.info("Already started this source ({})".format(source_name))
                    # we already started on this source... do nothing
                    return
                else:
                    self.log.warning("Trying to start a new observation before old one has been stopped...")
                    ## we need to stop and change to a new source!
                    self.abort_observation()
                    self.start_observation(source_name, observing_time)
                    return
            self.log.warning(f"Cannot start observation... not 'ready'. State is '{state['observation_status']}'")
            return

        self.update_state({'source_name':source_name})
        ## Assume for now we reset the cpu map each obesrvation.
        self.cpu_map = {}
        # @todo: Maybe more thought could be added here...
        self.cpu_map = self.digitiser_interface.get_cpu_map(cpu_map=self.cpu_map)
        self.cpu_map = self.dspsr.get_cpu_map(cpu_map=self.cpu_map)

        self.log.info(f"CPU Map: {self.cpu_map}")
        # Once the CPU map is set, we can actually start the obseving

        # Create the ringbuffers
        for kwargs in self.config['ringbuffers']:
            self.ringbuffer.destroy_buffer(kwargs['label'])
            self.ringbuffer.create_buffer(**kwargs)
        # Wait for the ringbuffers to start.
        self.ringbuffer.wait()
        # update the state
        state = self.get_state()
        for kwargs in self.config['ringbuffers']:
            if not state['ringbuffer'][kwargs['label']]['ready']:
                self.log.error("Could not start observation because a ringbuffer could not be created")
                return

        # Start dspsr
        # @todo: implement this...
        # Wait for dspsr to start...
        self.dspsr.start_observation()
        self.dspsr.wait()
        time.sleep(1)
        # @todo: check that dspsr started correctly.

        # Finally triger the start with the digitiser
        self.digitiser_interface.start_observation(observing_time=observing_time)
        self.digitiser_interface.wait()
        # @todo: Check that we have started?

        self.update_state({'observation_status': 'Observing'})

        pass

    @subcomponentmethod
    def abort_observation(self):
        self.update_state({'observation_status': 'stopping'})
        # stop the data stream
        self.digitiser_interface.abort_observation()
        # wait 10s or so for dspsr to finish...
        for tick in range(20):
            time.sleep(0.5)
            state = self.get_state()
            wait = False
            for k, v in state['dspsr']['processes'].items():
                if v == 'Running':
                    wait = True
            if not wait:
                break
        self.dspsr.abort_observation()
        self.update_state({'source_name': 'Unknown'})
        self.check_observing_state()

    def check_observing_state(self):
        state = self.get_state()
        detected_obs_state = "Invalid"
        if 'dspsr' not in state or 'roach2' not in state:
            self.update_state({'observation_status': detected_obs_state})
            return

        if state['dspsr']['state'] in ['Running','Completed'] and state['roach2']['state'] in ['Running','Completed']:
            detected_obs_state = 'Observing'

        if state['dspsr']['state'] in ['Completed','Idle'] and state['roach2']['state'] in ['Completed','Idle']:
            detected_obs_state = 'Completed'

        if state['dspsr']['state'] == 'Idle' and state['roach2']['state'] == 'Idle':
            detected_obs_state = 'Ready'

        if state['dspsr']['state'] == 'Error' or state['roach2']['state'] == 'Error':
            detected_obs_state = 'Error'

        self.update_state({'observation_status': detected_obs_state})

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
