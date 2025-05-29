# Socket Server Python
import socket
import threading
import socketserver

import sys
from PySide6.QtWidgets import QApplication, QWidget, QPushButton, QMessageBox, QLabel
from PySide6.QtWidgets import QListWidget
from PySide6.QtGui import QCloseEvent

# Commands for communicating with VideoStream over TCP/IP socket (port 4610)
def register_with_dserv(ipaddr, port):
    # Create a control socket for sending commands to VideoStream
    dserv_address = (ipaddr, 4620)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(dserv_address)
    cmd = f"%reg {sock.getsockname()[0]} {port}\r\n"

    sock.sendall(cmd.encode('UTF-8'))
    result = sock.makefile().readline()
    #print(result)
    
    sock.close()
    return

def add_match(ipaddr, port, varname):
    # Create a control socket for sending commands to VideoStream
    dserv_address = (ipaddr, 4620)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(dserv_address)

    cmd = f"%match {sock.getsockname()[0]} {port} {varname} 1\r\n"
    sock.sendall(cmd.encode('UTF-8'))
    result = sock.makefile().readline()
    #print(result)

    sock.close()
    return


class MyTCPHandler(socketserver.BaseRequestHandler):
    """
    The request handler class for our server.

    It is instantiated once per connection to the server, and must
    override the handle() method to implement communication to the
    client.
    """

    def handle(self):
        # self.rfile is a file-like object created by the handler;
        # we can now use e.g. readline() instead of raw recv() calls
        while True:
            self.data = self.request.makefile().readline().strip()
            ex.listbox.addItem(self.data)
#            ex.label.adjustSize()

class ExampleWindow(QWidget):
    def __init__(self):
        super().__init__()

        self.setup()

    def setup(self):

#        self.label = QLabel("", self)
        self.listbox = QListWidget(self)
        
        self.setGeometry(100, 100, 300, 150)
        self.setWindowTitle('dserv example')

        self.show()



def run():


    sys.exit(app.exec())


if __name__ == "__main__":
    HOST, PORT = "0.0.0.0", 0

    app = QApplication(sys.argv)
    ex = ExampleWindow()
    
    server = socketserver.TCPServer((HOST, PORT), MyTCPHandler)
    socketserver.window = ex

    ip, port = server.server_address
    t = threading.Thread(target=server.serve_forever)
    t.daemon = True
    t.start()
    
    register_with_dserv('127.0.0.1', port)
    match_arg = sys.argv[1] if len(sys.argv) > 1 else "eventlog/events"
    add_match('127.0.0.1', port, match_arg)
    run()

