#!/usr/bin/env python3
"""
dserv compute broker - manages AWS spot instances for on-demand compute

Production workflow:
- Multiple clients share single worker instance
- Clients call GET_WORKER before each remote operation
- PING keeps worker alive during long-running sessions
- Automatic timeout-based cleanup after idle period

Development workflow:
- RELEASE command available for immediate shutdown during testing
"""
import boto3
import socket
import time
import threading
from datetime import datetime

# Configuration
LISTEN_PORT = 9000
IDLE_TIMEOUT = 900  # 15 minutes (adjustable based on workflow)
AMI_ID = "ami-00320325883f696a9"
INSTANCE_TYPE = "c7i.16xlarge"  # 64 cores (or c7i.8xlarge for 32)
SECURITY_GROUP = "sg-OUR_SECURITY_GROUP" 
KEY_NAME = "dserv-workers"

class ComputeBroker:
    def __init__(self):
        self.ec2 = boto3.client('ec2')
        self.workers = {}  # {instance_id: {'ip': str, 'last_used': timestamp}}
        self.discover_existing_workers()
        
    def discover_existing_workers(self):
        """Find any running workers from previous broker sessions"""
        response = self.ec2.describe_instances(
            Filters=[
                {'Name': 'tag:Name', 'Values': ['dserv-worker']},
                {'Name': 'instance-state-name', 'Values': ['running', 'pending']}
            ]
        )
        
        for reservation in response['Reservations']:
            for instance in reservation['Instances']:
                instance_id = instance['InstanceId']
                ip = instance.get('PublicIpAddress')
                if ip:
                    self.workers[instance_id] = {
                        'ip': ip,
                        'last_used': time.time()  # Mark as fresh
                    }
                    print(f"Discovered existing worker {instance_id} at {ip}")
        
    def get_or_create_worker(self):
        """
        Return IP of current worker, launching new one if needed.
        All clients share the same worker instance.
        Resets idle timer on each call.
        """
        # Check for existing worker (shared across all clients)
        now = time.time()
        for instance_id, info in self.workers.items():
            if now - info['last_used'] < IDLE_TIMEOUT:
                info['last_used'] = now
                print(f"Returning shared worker {instance_id} at {info['ip']}")
                return info['ip']
        
        # No worker available, launch new one (client waits ~30-60s)
        print("No worker available, launching new instance...")
        return self.launch_worker()
    
    def launch_worker(self):
        """
        Launch a new worker instance with fallback strategy.
        Tries multiple instance types as spot, falls back to on-demand if needed.
        """
        # Fallback instance types (64 vCPU equivalents)
        spot_instance_types = [
            INSTANCE_TYPE,      # Primary choice
            'c6i.16xlarge',     # Previous gen Intel
            'c7a.16xlarge',     # AMD alternative
        ]
        
        # Try spot instances first
        for instance_type in spot_instance_types:
            try:
                print(f"Attempting to launch {instance_type} spot instance...")
                response = self.ec2.run_instances(
                    ImageId=AMI_ID,
                    InstanceType=instance_type,
                    KeyName=KEY_NAME,
                    SecurityGroupIds=[SECURITY_GROUP],  # Use ID instead of name
                    MinCount=1,
                    MaxCount=1,
                    InstanceMarketOptions={
                        'MarketType': 'spot',
                        'SpotOptions': {
                            'SpotInstanceType': 'one-time',
                            'InstanceInterruptionBehavior': 'terminate'
                        }
                    },
                    TagSpecifications=[{
                        'ResourceType': 'instance',
                        'Tags': [{'Key': 'Name', 'Value': 'dserv-worker'}]
                    }]
                )
                
                instance_id = response['Instances'][0]['InstanceId']
                print(f"Successfully launched {instance_type} spot instance {instance_id}")
                print(f"Waiting for IP...")
                
                # Wait for instance to be running and get IP
                ip = self.wait_for_instance(instance_id)
                
                self.workers[instance_id] = {
                    'ip': ip,
                    'last_used': time.time()
                }
                
                print(f"Worker {instance_id} ready at {ip}")
                return ip
                
            except Exception as e:
                error_msg = str(e)
                if 'InsufficientInstanceCapacity' in error_msg:
                    print(f"No spot capacity for {instance_type}, trying next option...")
                    continue
                elif 'SpotMaxPriceTooLow' in error_msg:
                    print(f"Spot price too high for {instance_type}, trying next option...")
                    continue
                else:
                    # Unexpected error, don't continue trying
                    print(f"Unexpected error launching {instance_type}: {error_msg}")
                    raise
        
        # All spot attempts failed, fall back to on-demand
        print("WARNING: All spot instances unavailable, falling back to on-demand")
        print("WARNING: This will cost approximately 3x more (~$2.50/hour vs ~$0.80/hour)")
        
        try:
            response = self.ec2.run_instances(
                ImageId=AMI_ID,
                InstanceType=INSTANCE_TYPE,  # Use primary instance type
                KeyName=KEY_NAME,
                SecurityGroupIds=[SECURITY_GROUP],  # Use ID instead of name
                MinCount=1,
                MaxCount=1,
                # No InstanceMarketOptions = on-demand
                TagSpecifications=[{
                    'ResourceType': 'instance',
                    'Tags': [
                        {'Key': 'Name', 'Value': 'dserv-worker'},
                        {'Key': 'Type', 'Value': 'on-demand'}  # Tag for cost tracking
                    ]
                }]
            )
            
            instance_id = response['Instances'][0]['InstanceId']
            print(f"Launched on-demand instance {instance_id}")
            print(f"Waiting for IP...")
            
            ip = self.wait_for_instance(instance_id)
            
            self.workers[instance_id] = {
                'ip': ip,
                'last_used': time.time()
            }
            
            print(f"On-demand worker {instance_id} ready at {ip}")
            return ip
            
        except Exception as e:
            print(f"Failed to launch on-demand instance: {e}")
            raise Exception("Unable to launch any worker instance (spot or on-demand)")
    
    def wait_for_instance(self, instance_id, timeout=120):
        """Wait for instance to be running and return public IP"""
        # Small initial delay to let AWS register the instance
        time.sleep(3)
        
        start = time.time()
        while time.time() - start < timeout:
            try:
                response = self.ec2.describe_instances(InstanceIds=[instance_id])
                instance = response['Reservations'][0]['Instances'][0]
                
                state = instance['State']['Name']
                if state == 'running':
                    ip = instance.get('PublicIpAddress')
                    if ip:
                        return ip
            except Exception as e:
                # Instance might not be immediately available, keep trying
                if 'InvalidInstanceID.NotFound' in str(e):
                    print(f"Instance not yet registered, waiting...")
                else:
                    raise
            
            time.sleep(2)
        
        raise TimeoutError(f"Instance {instance_id} didn't start in {timeout}s")
    
    def get_worker_status(self):
        """Return status of all known workers"""
        status = []
        for instance_id, info in self.workers.items():
            idle_time = time.time() - info['last_used']
            status.append({
                'instance_id': instance_id,
                'ip': info['ip'],
                'idle_seconds': int(idle_time),
                'will_terminate_in': max(0, int(IDLE_TIMEOUT - idle_time))
            })
        return status
    
    def list_all_running_instances(self):
        """Query AWS for all running dserv workers (including unknowns)"""
        try:
            response = self.ec2.describe_instances(
                Filters=[
                    {'Name': 'tag:Name', 'Values': ['dserv-worker']},
                    {'Name': 'instance-state-name', 'Values': ['running']}
                ]
            )
            
            instances = []
            for reservation in response['Reservations']:
                for instance in reservation['Instances']:
                    instances.append({
                        'instance_id': instance['InstanceId'],
                        'instance_type': instance['InstanceType'],
                        'ip': instance.get('PublicIpAddress', 'pending'),
                        'launch_time': instance['LaunchTime'].isoformat(),
                        'state': instance['State']['Name']
                    })
            return instances
        except Exception as e:
            return {'error': str(e)}
    
    def terminate_all_workers(self):
        """Terminate all known workers immediately"""
        if not self.workers:
            return {'message': 'No workers to terminate'}
        
        instance_ids = list(self.workers.keys())
        try:
            self.ec2.terminate_instances(InstanceIds=instance_ids)
            for instance_id in instance_ids:
                del self.workers[instance_id]
            return {'terminated': instance_ids}
        except Exception as e:
            return {'error': str(e)}
    
    def ping_worker(self, worker_ip):
        """Reset idle timer for worker (keeps it alive)"""
        for instance_id, info in self.workers.items():
            if info['ip'] == worker_ip:
                info['last_used'] = time.time()
                print(f"Ping received for worker {instance_id}, timer reset")
                return True
        print(f"Warning: Worker {worker_ip} not found for ping")
        return False
    
    def release_worker(self, worker_ip):
        """Terminate worker immediately (development/testing only)"""
        for instance_id, info in list(self.workers.items()):
            if info['ip'] == worker_ip:
                print(f"Releasing worker {instance_id} at {worker_ip} (explicit RELEASE)")
                self.ec2.terminate_instances(InstanceIds=[instance_id])
                del self.workers[instance_id]
                return
        print(f"Warning: Worker {worker_ip} not found")
    
    def cleanup_idle_workers(self):
        """Background thread to terminate idle workers after timeout"""
        while True:
            time.sleep(60)  # Check every minute
            now = time.time()
            to_terminate = []
            
            for instance_id, info in list(self.workers.items()):
                idle_time = now - info['last_used']
                if idle_time > IDLE_TIMEOUT:
                    to_terminate.append(instance_id)
                    print(f"Worker {instance_id} idle for {idle_time:.0f}s (timeout: {IDLE_TIMEOUT}s)")
            
            if to_terminate:
                print(f"Terminating idle workers: {to_terminate}")
                self.ec2.terminate_instances(InstanceIds=to_terminate)
                for instance_id in to_terminate:
                    del self.workers[instance_id]
    
    def handle_client(self, conn, addr):
        """Handle client connection - process commands"""
        try:
            print(f"Client connected from {addr}")
            
            # Read command from client
            data = conn.recv(1024).decode().strip()
            
            if data == "GET_WORKER" or data == "":
                # Request for worker IP (shares worker, resets timer)
                worker_ip = self.get_or_create_worker()
                conn.send(f"{worker_ip}\n".encode())
            
            elif data.startswith("PING "):
                # Keep worker alive (resets timer)
                worker_ip = data.split()[1]
                if self.ping_worker(worker_ip):
                    conn.send(b"OK\n")
                else:
                    conn.send(b"ERROR: Worker not found\n")
            
            elif data == "STATUS":
                # Get status of known workers
                import json
                status = self.get_worker_status()
                conn.send(json.dumps(status).encode() + b"\n")
            
            elif data == "LIST_ALL":
                # Query AWS for all running instances
                import json
                instances = self.list_all_running_instances()
                conn.send(json.dumps(instances).encode() + b"\n")
            
            elif data == "TERMINATE_ALL":
                # Terminate all known workers
                import json
                result = self.terminate_all_workers()
                conn.send(json.dumps(result).encode() + b"\n")
            
            elif data.startswith("RELEASE "):
                # Immediate shutdown (development/testing only)
                worker_ip = data.split()[1]
                self.release_worker(worker_ip)
                conn.send(b"OK\n")
            
            else:
                conn.send(b"ERROR: Unknown command\n")
                
        except Exception as e:
            print(f"Error handling client: {e}")
            conn.send(b"ERROR\n")
        finally:
            conn.close()
    
    def run(self):
        """Main broker loop"""
        # Start cleanup thread
        cleanup_thread = threading.Thread(target=self.cleanup_idle_workers, daemon=True)
        cleanup_thread.start()
        
        # Listen for clients
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('0.0.0.0', LISTEN_PORT))
        sock.listen(5)
        
        print(f"Broker listening on port {LISTEN_PORT}")
        print(f"Configuration: {INSTANCE_TYPE}, timeout={IDLE_TIMEOUT}s")
        
        while True:
            conn, addr = sock.accept()
            # Handle in thread so we don't block other clients during launch
            threading.Thread(target=self.handle_client, args=(conn, addr)).start()

if __name__ == '__main__':
    broker = ComputeBroker()
    broker.run()
