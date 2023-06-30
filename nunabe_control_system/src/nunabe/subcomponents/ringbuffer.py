import subprocess
import logging

from ..subcomponent import SubComponent, subcomponentmethod


class Ringbuffer(SubComponent):

    def __init__(self, backend):
        """
        This is the ringbuffer subcomponent. It is responsible for
             * Creating the dada ringbuffer
             * Monitoring the ringbuffer status
             * Destroying the ringbuffer
         """
        super().__init__(looptime=0.5)
        self.backend = backend
        self.keys = {}
        self.states = {}
        self.log = logging.getLogger("nunabe.ringbuffer")

    @subcomponentmethod
    def create_buffer(self, label, key, bufsz=524288, hdrsz=4096, nbufs=128):
        key = str(key)
        self.log.info(f"create ringbuffer {label} / {key}")
        if label in self.states:
            self.log.warning(f"Ringbuffer {label} already exists, ignoring...")
            return
        cmd = [str(i) for i in ['dada_db', '-k', key, '-b', bufsz, '-a', hdrsz, '-n', nbufs]]
        self.keys[label] = key
        self.states[label] = newstate(key)
        try:
            self.log.info("! " + " ".join(cmd))
            ret = subprocess.run(cmd, timeout=1.0)
        except subprocess.TimeoutExpired:
            self.log.error("dada_db timed out creating ringbuffer")
            self.states[label]['error'] = 'Could not create: Timeout'
            return
        if ret.returncode == 0:
            ## all good!
            self.log.info(f"buffer {key} created for {label}")
            self.states[label]['hdrsz'] = hdrsz
            self.states[label]['nbufs'] = nbufs
            self.states[label]['bufzs'] = bufsz
            self.states[label]['ready'] = True
        else:
            ## dada_db threw an error.
            self.log.error(f"dada_db could not create ringbuffer exit={ret.returncode}")
            self.states[label]['error'] = 'Could not create'

    @subcomponentmethod
    def destroy_buffer(self, label):
        self.log.info(f"destroy ringbuffer {label}")
        key = self.keys[label]
        self.states[label]['hdrsz'] = 0
        self.states[label]['nbufs'] = 0
        self.states[label]['bufzs'] = 0
        self.states[label]['ready'] = False
        cmd = ['dada_db', '-k', key, '-d']
        try:
            self.log.info("! " + " ".join(cmd))
            ret = subprocess.run(cmd, timeout=1.0)
        except subprocess.TimeoutExpired:
            self.backend.logerr("dada_db timed out destroying ringbuffer")
            self.states[label]['error'] = 'Could not destroy: Timeout'
            return
        if ret.returncode == 0:
            ## all good!
            self.log.info(f"buffer {key} destroyed for {label}")
        else:
            ## dada_db threw an error.
            self.log.info(f"dada_db could not destroy ringbuffer exit={ret.returncode}")
            self.states[label]['error'] = 'Could not destroy'

    def loop(self):
        """
        This routine is called every loop of this subcomponent
        """
        super().loop()
        self.log.debug("RING LOOP")

        for label in self.states:
            if self.states[label]['ready']:
                cmd = ['dada_dbmetric', '-k', self.states[label]['key']]
                try:
                    self.log.info("! " + " ".join(cmd))
                    ret = subprocess.run(cmd, timeout=0.1, encoding='utf-8',capture_output=True)
                    self.log.debug(f"dada_dbmetric: '{ret.stderr}'")

                    total, full, clear, _, _, _, _, _, _, _ = [int(e) for e in ret.stderr.split(",")]
                    self.states[label]['nbufs'] = total
                    self.states[label]['clear'] = clear
                    self.states[label]['full'] = full
                    self.states[label]['used'] = full / total

                except subprocess.TimeoutExpired:
                    self.backend.logerr("dada_dbmetric timed out")
                    self.states[label]['error'] = 'Could not monitor: Timeout'

        state = {'ringbuffer': self.states}
        self.backend.update_state(state)

    def handle_exception(self, e):
        self.log.critical(f"Exception raised!! '{e}'")

    def stop(self):
        """
        This routine is called at the termination of this subcomponent.
        Clean up memory!
        """
        self.log.info("STOP requested... clean up memory")
        for k in self.keys:
            self.destroy_buffer(k)
        super().stop()

    def final(self):
        self.log.info("Ringbuffer Process Stopped")

    def start(self):
        super().start()
        self.log.info("Ringbuffer Process Started")


def newstate(key):
    return {'key': key,
            'bufsz': 0,
            'nbufs': 0,
            'hdrsz': 0,
            'full': 0,
            'used': 0.0,
            'clear': 0,
            'error': '',
            'ready': False}
