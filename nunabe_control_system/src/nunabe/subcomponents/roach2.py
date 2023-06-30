import subprocess
import logging
import time

from ..subcomponent import SubComponent, subcomponentmethod

import uuid
import os


class Roach2(SubComponent):
    """
    Functions of the ROACH2 are
    Reprogramme. Possibly choose mode?

    Trigger on 1PPS to kick off a new observation

    Handle the roach2udpdb instances.
    """

    def __init__(self, backend):
        """
        This is the Roach Interface subcomponet. It is responsible with organising data streaming from the Roach into the ringbuffer
         """
        super().__init__(looptime=0.2)
        self.backend = backend
        self.log = logging.getLogger("nunabe.roach2")

        self.uuid = str(uuid.uuid4())
        self.uwd = os.path.join("/tmp", self.uuid)
        os.path.mkdir(self.uwd)

        self.low_chans_config = {'addr': '10.0.3.1', 'port': 60000, 'ctl_fifo': f'{self.uwd}/low_chans_control_fifo',
                                 'mon_fifo': f'{self.uwd}/low_chans_monitor_fifo'}
        self.high_chans_config = {'addr': '10.0.3.2', 'port': 60000, 'ctl_fifo': f'{self.uwd}/high_chans_control_fifo',
                                  'mon_fifo': f'{self.uwd}/high_chans_monitor_fifo'}
        self.full_bandwidth = 512
        self.inverted_frequencies = -1

        self.state = {'error': "",
                      'band_select':-1,
                      'status': 'idle'}

    @subcomponentmethod
    def reprogram(self, band_select):
        if band_select in [0, 2, 4, 6, 8, 10, 12, 14]:
            self.nchan = 32 - 2 * band_select
            self.bandwidth = self.nchan * (self.full_bandwidth / 32)
        else:
            self.log.critical(f"Invalid band_select chosen {band_select}")

        cmd = ['sudo', '/opt/roach2_control', f"{band_select}"]
        try:
            self.log.info("! " + " ".join(cmd))
            ret = subprocess.run(cmd, timeout=30.0)  # allow 30s for it to program
        except subprocess.TimeoutExpired:
            self.log.error("Timeout trying to program roach2")
            self.state['error'] = 'Could not program roach (timeout)'
            return
        if ret.returncode == 0:
            ## all good!
            self.log.info(f"Roach2 programed ok")
            self.state['band_select'] = band_select
            self.state['status'] = "ready"
        else:
            ## dada_db threw an error.
            self.log.error("Error trying to program roach2...")
            self.state['error'] = 'Could not program roach2'

        return

    @subcomponentmethod
    def start_observation(self, tobs):
        state = self.backend.state
        # source name,  centre freq are in the state.
        centre_freq = state['centre_freq']

        half_bandwidth = self.inverted_frequencies * self.bandwidth / 2
        low_chan_centre_freq = centre_freq - half_bandwidth / 2
        high_chan_centre_freq = centre_freq + half_bandwidth / 2

        self.log.info(
            f"Low chans: {self.low_chans_config['addr']}:{self.low_chans_config['port']}  CtrFrq: {low_chan_centre_freq} MHz BW:{half_bandwidth} MHz")
        self.log.info(
            f"High chans: {self.high_chans_config['addr']}:{self.high_chans_config['port']}  CtrFrq: {high_chan_centre_freq} MHz BW:{half_bandwidth} MHz")

        def getopts(config, freq, bw):
            return ['-I', config['addr'],
                    '-p', config['port'],
                    '-C', config['ctl_fifo'],
                    '-M', config['mon_fifo'],
                    '-f', freq,
                    '-b', bw,
                    '-T', tobs]

        # Start the roach2_udpdb programmes to listen.
        cpu_low_rx = self.backend.cpu_map{'cpu_low_rx'}


        return

    @subcomponentmethod
    def abort_observation(self):
        return

    def loop(self):
        super().loop()
        print("ROACH LOOP")
        self.backend.update_state({"roach2": self.state})

    def stop(self):
        self.backend.log.info("Stopping ROACH interface")
        super().stop()

    def final(self):
        super().final()


    def get_cpu_map(self):
        # @TODO: read this from the configuration somehow.
        interfaces=['ens1f0','ens1f1'] # Would be nice not to hardcode this
        # Find out which cpus the kernel is using to capture packets.
        with open("/proc/interrupts") as f:
            cpu_names = f.readline().split()
            ncpu=len(cpu_names)
            counts = {}
            for ifce in interfaces:
                counts[ifce] = [0 for i in range(ncpu)]
            for line in f:
                e=line.split()
                for ifce in interfaces:
                    if ifce in e[-1]:
                        for cpu in range(ncpu):
                            # we negatively count the current value
                            counts[ifce][cpu] -= int(e[cpu+1])

        time.sleep(0.5)
        with open("/proc/interrupts") as f:
            hdr = f.readline().split()
            for line in f:
                e = line.split()
                for ifce in interfaces:
                    if ifce in e[-1]:
                        for cpu in range(ncpu):
                            counts[ifce][cpu] += int(e[cpu + 1])

        # counts now has the number of events that we saw

        cpu_map = {}
        for ifce in interfaces:
            bestcpu = counts[ifce].index(min(counts[ifce]))
            cpu_map[bestcpu] = f'kernel_{ifce}'

        return cpu_map


