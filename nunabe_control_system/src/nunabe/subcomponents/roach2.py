import shutil
import subprocess
import logging
import time
import math

from .kill_processes import kill_processes
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
        self.high_proc = None
        self.low_proc = None
        self.mon_fifo = {}
        self.ctl_fifo = {}
        self.backend = backend
        self.log = logging.getLogger("nunabe.roach2")

        self.full_bandwidth = 512
        self.inverted_frequencies = -1

        self.state = {'error': "",
                      'band_select': -1,
                      'roach2status': 'Not Programmed',
                      'udpdb_low': 'Stopped',
                      'udpdb_high': 'Stopped',
                      'state': 'Idle'}

    @subcomponentmethod
    def reprogram(self, band_select, dont_actually_program=False):
        if band_select in [0, 2, 4, 6, 8, 10, 12, 14]:
            self.nchan = 32 - 2 * band_select
            self.bandwidth = self.nchan * (self.full_bandwidth / 32)
        else:
            self.log.critical(f"Invalid band_select chosen {band_select}")

        if dont_actually_program:
            self.state['band_select'] = band_select
            return

        cmd = ['sudo', self.backend.config['roach2_settings']['roach2_reprogram_script'], f"{band_select}"]
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
            self.state['roach2status'] = "Programmed"
        else:
            ## dada_db threw an error.
            self.log.error("Error trying to program roach2...")
            self.state['error'] = 'Could not program roach2'
            self.state['roach2status'] = "Error"

        self.backend.update_state({'roach2': self.state})
        return

    @subcomponentmethod
    def start_observation(self, observing_time):
        self.close_pipes()
        low_chans_config = self.backend.config['roach2_settings']['low_chans_config']
        high_chans_config = self.backend.config['roach2_settings']['high_chans_config']

        # source name,  centre freq are in the state.
        centre_freq = self.backend.config['telescope_settings']['centre_freq']

        half_bandwidth = self.inverted_frequencies * self.bandwidth / 2
        low_chan_centre_freq = centre_freq - half_bandwidth / 2
        high_chan_centre_freq = centre_freq + half_bandwidth / 2

        self.log.info(
            f"Low chans: {low_chans_config['addr']}:{low_chans_config['port']}  CtrFrq: {low_chan_centre_freq} MHz BW:{half_bandwidth} MHz")
        self.log.info(
            f"High chans: {high_chans_config['addr']}:{high_chans_config['port']}  CtrFrq: {high_chan_centre_freq} MHz BW:{half_bandwidth} MHz")

        inv_cpu_map = dict((v, k) for k, v in self.backend.cpu_map.items())
        roach2_udpdb = self.backend.config['roach2_settings']['roach2_udpdb']

        def get_commandline(config, freq, bw):
            ifce = config['interface']
            socket_cpu = inv_cpu_map[f"roach2_socket_thread_{ifce}"]
            dada_cpu = inv_cpu_map[f"roach2_dada_thread_{ifce}"]
            nice = config['priority']
            ctl_fifo = os.path.join(self.uwd, config['ctl_fifo'])
            mon_fifo = os.path.join(self.uwd, config['mon_fifo'])
            # In case we somehow already have a pipe... try to delete it
            if os.path.exists(ctl_fifo):
                os.unlink(ctl_fifo)
            if os.path.exists(mon_fifo):
                os.unlink(mon_fifo)
            # make the named pipes for use later
            os.mkfifo(ctl_fifo)
            os.mkfifo(mon_fifo)

            cmd = ['nice', '-n', str(nice),
                   'taskset', '-c', str(dada_cpu),
                   roach2_udpdb,
                   '-I', config['addr'],
                   '-p', str(config['port']),
                   '-k', config['dada']['key'],
                   '-C', ctl_fifo,
                   '-M', mon_fifo,
                   '-f', str(freq),
                   '-b', str(bw),
                   '-c', str(socket_cpu),
                   '-T', str(observing_time)]
            cmd.extend(config['extra_cmd_options'])
            return cmd, ctl_fifo, mon_fifo

        # Start the roach2_udpdb programmes to listen.

        low_cmd, low_ctl_fifo_f, low_mon_fifo_f = get_commandline(low_chans_config, low_chan_centre_freq,
                                                                  half_bandwidth)
        high_cmd, high_ctl_fifo_f, high_mon_fifo_f = get_commandline(high_chans_config, high_chan_centre_freq,
                                                                     half_bandwidth)

        self.log.info(f"Starting {roach2_udpdb}")
        self.log.info("! " + " ".join(low_cmd))
        self.low_proc = subprocess.Popen(low_cmd)

        self.log.info("! " + " ".join(high_cmd))
        self.high_proc = subprocess.Popen(high_cmd) 

        self.mon_fifo = dict(low=os.fdopen(os.open(low_mon_fifo_f, os.O_RDONLY | os.O_NONBLOCK)),
                             high=os.fdopen(os.open(high_mon_fifo_f, os.O_RDONLY | os.O_NONBLOCK)))
        self.ctl_fifo = dict(low=open(low_ctl_fifo_f, 'w'), high=open(high_ctl_fifo_f, 'w'))

        self.state['udpdb_low'] = 'Launched'
        self.state['udpdb_high'] = 'Launched'
        self.state['state'] = 'Running'
        self.backend.update_state({'roach2': self.state})

        ## @todo Triger the 1pps!

        return

    @subcomponentmethod
    def abort_observation(self):
        # @todo: Tell the data stream to stop...
        self.cleanup_observation()
        return

    @subcomponentmethod
    def cleanup_observation(self):
        kill_processes([self.low_proc,self.high_proc]) 
        
        self.close_pipes()

        self.state['udpdb_low'] = 'Stopped'
        self.state['udpdb_high'] = 'Stopped'
        self.state['state'] = 'Idle'
        self.backend.update_state({'roach2': self.state})

        return

    

    def close_pipes(self):
        # Close the pipes...
        for pair_of_pipes in zip(self.mon_fifo.values(), self.ctl_fifo.values()):
            for pipe in pair_of_pipes:
                try:
                    pipe.close()
                except:
                    pass
                try:
                    os.unlink(pipe.name)
                except:
                    pass
            self.mon_fifo = {}
            self.ctl_fifo = {}

    def loop(self):
        super().loop()
        if self.state['state'] == 'Running':
            # We should be observing!
            # state,
            # context->packet_count, context->dropped_packets,
            # context->block_count, context->packets_to_read, context->seconds_per_packet,
            # context->buffer_lag, context->max_buffer_lag, context->recent_buffer_lag,context->number_of_overruns,
            # NUM_PACKET_BUFFERS);
            for key in self.mon_fifo:
                while line := self.mon_fifo[key].readline():
                    e = line.split()
                    state = e[0]
                    packet_count = int(e[1])
                    dropped_packets = int(e[2])
                    block_count = int(e[3])
                    packets_to_read = int(e[4])
                    seconds_per_packet = float(e[5])
                    buffer_lag = int(e[6])
                    max_buffer_lag = int(e[7])
                    recent_buffer_lag = int(e[8])
                    number_of_overruns = int(e[9])
                    buffer_size = int(e[10])
                    self.state[f'udpdb_{key}'] = state
                    self.state[f'udpdb_progress_{key}'] = dict(recorded=seconds_per_packet * packet_count,
                                                               remaining=seconds_per_packet * (
                                                                       packets_to_read - packet_count))
                    self.state[f'udpdb_buffer_{key}'] = dict(buffer_lag=buffer_lag, max_buffer_lag=max_buffer_lag,
                                                             recent_buffer_lag=recent_buffer_lag,
                                                             number_of_overruns=number_of_overruns,
                                                             buffer_size=buffer_size)
                    self.state[f'udpdb_packets_{key}'] = dict(packet_count=packet_count,
                                                              dropped_packets=dropped_packets,
                                                              block_count=block_count, packets_to_read=packets_to_read,
                                                              seconds_per_packet=seconds_per_packet)
                    self.log.info(
                        f"{state} ({key}) {seconds_per_packet * packet_count}s Dropped packets: {dropped_packets} Overruns: {number_of_overruns}")

        self.backend.update_state({"roach2": self.state})

    def start(self):
        super().start()
        self.uuid = str(uuid.uuid4())
        self.uwd = os.path.join("/tmp", f"nunabe_roach2_{self.uuid}")
        os.makedirs(self.uwd)

    def stop(self):
        self.backend.log.info("Stopping ROACH interface")
        self.abort_observation()
        try:
            shutil.rmtree(self.uwd)
        except IOError:
            pass
        super().stop()

    def final(self):
        super().final()

    def get_cpu_map(self, cpu_map):

        cpu_map = cpu_map.copy()

        interfaces = self.backend.config['roach2_settings']['interfaces']
        # Find out which cpus the kernel is using to capture packets.
        with open("/proc/interrupts") as f:
            cpu_names = f.readline().split()
            ncpu = len(cpu_names)
            counts = {}
            for ifce in interfaces:
                counts[ifce] = [0 for i in range(ncpu)]
            for line in f:
                e = line.split()
                for ifce in interfaces:
                    if ifce in e[-1]:
                        for cpu in range(ncpu):
                            # we negatively count the current value
                            counts[ifce][cpu] -= int(e[cpu + 1])

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
        for ifce in interfaces:
            for icpu, count in enumerate(counts[ifce]):
                self.log.debug(f"{ifce} : [{icpu}] {count}")
            bestcpu = counts[ifce].index(max(counts[ifce]))
            if bestcpu in cpu_map:
                self.log.warning(
                    f'kernel thread for interface {ifce} already reserved for another process {cpu_map[bestcpu]}')
            cpu_map[bestcpu] = f'kernel_{ifce}'
            with open(f"/sys/class/net/{ifce}/device/local_cpus") as f:
                local_cpu_mask_int = int(f.readline(), 16)
            need_to_allocate_cores = [f'roach2_socket_thread_{ifce}', f'roach2_dada_thread_{ifce}']
            for icpu in range(self.backend.config['system_settings']['ncpu']):
                if need_to_allocate_cores:
                    if (local_cpu_mask_int >> icpu) & 0x1 == 1 and icpu not in cpu_map:
                        # This cpu is in the mask and we don't already have something allocated
                        cpu_map[icpu] = need_to_allocate_cores.pop()
            if need_to_allocate_cores:
                self.log.error(f"Could not allocated enough cpu cores for roach2 on {ifce}")
                for icpu in range(self.backend.config['system_settings']['ncpu']):
                    if need_to_allocate_cores and icpu not in cpu_map:
                        cpu_map[icpu] = need_to_allocate_cores.pop()

        self.log.debug(f"cpu_map: {cpu_map}")
        return cpu_map
