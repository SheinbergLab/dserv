import socket
import time
import threading
import queue
import json
from dataclasses import dataclass

@dataclass
class DelayedResponse:
    msg: str
    addr: tuple
    send_time: float
    
class DelayedResponseUDPServer:
    def __init__(self, host='localhost', port=8080, max_rate=100):
        self.host = host
        self.port = port
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.max_rate = max_rate
        self.min_interval = 1.0 / max_rate  # 0.01 seconds for 100/sec
        self.response_queue = queue.Queue()
        self.running = False
        self.last_send_time = 0
        self.lock = threading.Lock()
    
    def calculate_delay(self):
        """Calculate how long to wait before next response."""
        with self.lock:
            current_time = time.time()
            time_since_last = current_time - self.last_send_time
            
            if time_since_last >= self.min_interval:
                # Can send immediately
                self.last_send_time = current_time
                return 0
            else:
                # Need to wait
                delay = self.min_interval - time_since_last
                self.last_send_time = current_time + delay
                return delay
    
    def response_worker(self):
        """Worker thread that processes the response queue with delays."""
        while self.running:
            try:
                # Get next response to send
                response = self.response_queue.get(timeout=1)
                if response is None:  # Shutdown signal
                    break
                
                # Calculate and apply delay
                delay = self.calculate_delay()
                if delay > 0:
#                    print(f"Delaying response to {response.addr} by {delay:.3f}s")
                    time.sleep(delay)
                
                # Send the response
                self.socket.sendto(response.msg.encode(), response.addr)
#                print(f"Sent delayed response to {response.addr}")
                
                self.response_queue.task_done()
                
            except queue.Empty:
                continue
            except Exception as e:
                print(f"Response worker error: {e}")
    
    def start(self):
        self.socket.bind((self.host, self.port))
        self.running = True
        
        # Start response worker thread
        self.worker_thread = threading.Thread(target=self.response_worker, daemon=True)
        self.worker_thread.start()
        
        print(f"UDP Server listening on {self.host}:{self.port}")
        print(f"Rate limited to {self.max_rate} responses/second with processing delay")
        
        while self.running:
            try:
                data, addr = self.socket.recvfrom(1024)

                if data.decode() == "WAITFORDATA":
                    # Queue the response for delayed processing
                    current_time = time.time()
                    msg = json.dumps({"status": "success",
                                      "message": "Hello from server"})
                    response = DelayedResponse(msg, addr, current_time)
                    self.response_queue.put(response)
#                    print(f"Queued response for {addr} (queue size: {self.response_queue.qsize()})")
                
            except Exception as e:
                print(f"Error: {e}")
                break
    
    def stop(self):
        print("Stopping server...")
        self.running = False
        
        # Signal worker thread to stop
        self.response_queue.put(None)
        
        # Wait for pending responses to be sent
        print("Waiting for pending responses...")
        self.response_queue.join()
        
        if hasattr(self, 'worker_thread'):
            self.worker_thread.join(timeout=2)
        
        self.socket.close()
        print("Server stopped")

if __name__ == "__main__":
    # Choose implementation:
    server = DelayedResponseUDPServer(max_rate=100)  # Fixed interval delays
    
    try:
        server.start()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
        

        
