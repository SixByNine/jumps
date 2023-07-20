import os
import subprocess
import logging

from .kill_processes import kill_processes
from ..subcomponent import SubComponent, subcomponentmethod


class Dspsr(SubComponent):

    def __init__(self, backend):
        """
        This is the dspsr subcomponent. It is responsible for firing up dspsr and keeping an eye on it.
         """
        super().__init__(looptime=1.0)
        self.backend = backend
        self.log = logging.getLogger("nunabe.dspsr")

        self.processes = {}
        self.state = dict(error="", state="Idle",processes={})

    def start(self):
        super().start()
        self.backend.update_state({'dspsr': self.state})

    def loop(self):
        super().loop()
        errors = 0
        completed = 0
        for key,proc in self.processes.items():
            ret = proc.poll()
            if ret is None:
                self.state['processes'][key] = 'Running'
            else:
                if ret==0:
                    self.state['processes'][key] = 'Completed'
                    completed+=1
                else:
                    self.state['processes'][key] = 'Error'
                    errors+=1
        if errors > 0:
            self.state['state']='Error'
        if completed and completed == len(self.processes):
            self.state['state']='Completed'
        self.backend.update_state({'dspsr': self.state})

    def stop(self):
        super().stop()
        self.abort_observation()

    def final(self):
        super().final()

    @subcomponentmethod
    def start_observation(self):
        kill_processes(self.processes.values())
        self.processes = {}
        state = self.backend.get_state()
        source_name = state['source_name']  # Will get set by the telescope interface and/or user interface

        for key in self.backend.config['dspsr']:
            config = self.backend.config['dspsr'][key]

            data_path = config['data_root']  # Obviously needs some more stuff here.
            dada_key = config['dada']['key']
            info_file = os.path.join(data_path, 'dada.info')

            cpus = []
            for cpu in self.backend.cpu_map:
                if self.backend.cpu_map[cpu] == f"dspsr_{key}":
                    cpus.append(str(cpu))


            cmd = ['nice', '-n', str(config['priority'])]
            if cpus:
                cpustr = ",".join(cpus)
                cmd.extend(['taskset', '-c', cpustr])

            cmd.extend([config['dspsr'],
                        '-L', config['subint_seconds'],
                        '-F', f"{config['nchan']}:D",
                        '-N', source_name
                        ])

            if config['cuda'] is not None:
                cmd.extend(['-cuda', str(config['cuda'])])
            if config['threads'] is not None:
                cmd.extend(['-t',str(config['threads'])])
            cmd.extend(config['options'])
            cmd.extend(config['skz_options'])

            cmd.append(info_file)

            cmd = [str(i) for i in cmd]  # Make sure everything is a string!


            with open(info_file,'w') as dada_info_file:
                dada_info_file.write("DADA INFO:\n")
                dada_info_file.write(f"key {dada_key}:\n")

            self.log.info(f"Starting dspsr ({key})")
            self.log.info("! " + " ".join(cmd))
            self.processes[key] = subprocess.Popen(cmd,cwd=data_path)
            self.state['processes'][key] = 'Started'
            self.state['state'] = "Running"

    @subcomponentmethod
    def abort_observation(self):
        self.cleanup_observation()
        return

    @subcomponentmethod
    def cleanup_observation(self):
        kill_processes(self.processes.values())
        self.processes = {}
        self.state['state'] = "Idle"

    def get_cpu_map(self, cpu_map):
        cpu_map = cpu_map.copy()

        cmd = ['nvidia-smi', '--query-gpu=gpu_bus_id', '--format=csv,noheader,nounits']
        self.log.info("! " + " ".join(cmd))

        ret = subprocess.run(cmd, timeout=5.0, encoding='utf-8', capture_output=True)
        lines = ret.stdout.splitlines(keepends=False)
        self.log.debug(f"nvidia-smi {lines}")
        cuda_bus_locations = []
        for i, l in enumerate(lines):
            e = l.split(":")
            domain = int(e[0], 16)
            bus = int(e[1], 16)
            cuda_bus_locations.append((domain, bus))

        self.log.debug(f"cuda bus locations {cuda_bus_locations}")

        config = self.backend.config['dspsr']
        for id in config:
            cuda_id = config[id]['cuda']
            self.log.debug(f"Finding cpu for dspsr {id} on cuda device {cuda_id}")
            if cuda_id is not None:
                domain, bus = cuda_bus_locations[cuda_id]
                with open(f"/sys/class/pci_bus/{domain:04x}:{bus:02x}/cpuaffinity") as f:
                    local_cpu_mask_int = int(f.readline(), 16)
            else:
                local_cpu_mask_int = 0xffffffff

            threads = config[id]['threads'] if config[id]['threads'] else 2
            need_to_allocate_cores = [f"dspsr_{id}"] * threads
            for icpu in range(self.backend.config['system_settings']['ncpu']):
                if need_to_allocate_cores:
                    if (local_cpu_mask_int >> icpu) & 0x1 == 1 and icpu not in cpu_map:
                        # This cpu is in the mask, and we don't already have something allocated
                        cpu_map[icpu] = need_to_allocate_cores.pop()
            if need_to_allocate_cores:
                self.log.error(f"Could not allocated enough cpu cores for dspsr {id}")
                for icpu in range(self.backend.config['system_settings']['ncpu']):
                    if need_to_allocate_cores and icpu not in cpu_map:
                        cpu_map[icpu] = need_to_allocate_cores.pop()

        return cpu_map
