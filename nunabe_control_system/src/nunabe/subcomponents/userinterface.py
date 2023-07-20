import subprocess
from ..subcomponent import SubComponent, subcomponentmethod
import logging
import socket
import selectors
import queue
import time
import json


class UserInterface(SubComponent):

    def __init__(self, backend):
        """
        This is the UI subcomponent. It is responsible for receiving instructions from users
         """
        super().__init__(looptime=0)  ## This task must not wait Instead it will await input on the socket
        self.backend = backend
        self.log = logging.getLogger("nunabe.UI")
        self.selector = None
        self.sock = None
        self.connections=[]

    def loop(self):
        super().loop()
        if self.selector is None:
            ## Sockets not open yet!
            time.sleep(0.1)
        else:
            events = self.selector.select(timeout=0.1)
            for key, mask in events:
                if key.data is None:
                    conn, addr = self.sock.accept()
                    conn.setblocking(False)
                    connection = ui_connection(conn, addr, self)
                    self.connections.append(connection)
                    self.selector.register(conn, selectors.EVENT_READ, data=connection)
                else:
                    key.data.process()

    def stop(self):
        super().stop()
        self.log.info("Stopping UI")
        for connection in self.connections.copy():
            connection.close()

        if self.selector is not None:
            s = self.selector
            self.selector = None
            time.sleep(0.1)
            s.close()
        if self.sock is not None:
            self.sock.close()
        time.sleep(0.1)

    def final(self):
        super().final()

    def start(self):
        super().start()
        host = '127.0.0.1'
        port = 16942
        self.selector = selectors.DefaultSelector()
        self.log.info(f"Start UI server {host} {port}")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.sock.bind((host, port))
        self.sock.setblocking(False)
        self.sock.listen()
        self.selector.register(self.sock, selectors.EVENT_READ, data=None)


    @subcomponentmethod
    def parse_message(self, message, connection):
        self.log.info(f"Rx: {message}")
        if message=="STATE":
            self.get_state(connection, as_json=False)
        elif message=="JSON":
            self.get_state(connection,as_json=True)
        elif message == "SHUTDOWN":
            connection.write("OK -- requesting shutdown")
            self.backend.shutdown()
        elif message.startswith("STARTOBS"):
            e=message.split()
            if len(e) != 3:
                connection.write("ERROR -- STARTOBS source_name tobs")
                return
            source_name=e[1]
            tobs=float(e[2])
            connection.write("OK -- requesting observation start")
            self.backend.start_observation(source_name,tobs)
        elif message.startswith("STOPOBS"):
            connection.write("OK -- requesting observation stop")
            self.backend.abort_observation()
        else:
            connection.write("ERROR")

    @subcomponentmethod
    def get_state(self, connection, as_json=True):
        be_state = self.backend.get_state()
        if as_json:
            connection.write(json.dumps(be_state))
        else:
            # Pretty print?
            connection.write(str(be_state))



class ui_connection:
    def __init__(self, conn, client,ui_module):
        self.client = client
        self.conn = conn
        self.write_queue = queue.Queue()
        self.write_queue.put("CONNECTED\n")
        self.read_buffer = []
        self.ui_module=ui_module

    def close(self):
        self.ui_module.connections.remove(self)
        self.ui_module.selector.unregister(self.conn)
        self.conn.close()
    def process(self):
        data = self.conn.recv(1024)
        if data:
            data_string = data.decode('utf-8')
            if '\n' not in data_string:
                ## Case we got an incomplete line...
                self.read_buffer.append(data_string)
            else:
                # we got at least one endline
                e = data_string.split('\n')
                self.read_buffer.append(e[0])
                message = "".join(self.read_buffer)
                self.read_buffer = []
                # parse message
                self.ui_module.parse_message(message,self)
                for message in e[1:-1]:
                    # Another message got read somehow
                    self.ui_module.parse_message(message,self)
                if e[-1]:
                    # There is a left over fragment of a message
                    self.read_buffer.append(e[-1])
            # do something
        else:
            self.close()

    def write(self, message):
        message = message+"\n"
        self.conn.sendall(message.encode("utf-8"))
