# AWS Compute Broker Setup Guide

## Overview

This guide documents the setup of an on-demand compute system using AWS EC2 Spot instances for embarrassingly parallel workloads. The system allows dserv clients to request high-CPU compute workers (32 or 64 cores) that spin up on demand and automatically terminate when idle.

## Architecture

```
Tcl Client (local machine)
    ↓
Broker (always-on VPS - Hostinger/CloudFanatic)
    ↓ (AWS API calls via boto3)
AWS EC2 Spot Instances (compute workers)
    ↓
Client connects directly to worker IP:2560
```

**Key features:**
- Workers boot from custom AMI with dserv pre-installed (~20-30 seconds)
- Spot pricing: 60-90% discount vs on-demand
- Automatic idle timeout (15 minutes) or explicit release
- Linear scaling across all cores for embarrassingly parallel workloads

## Cost Estimates

**Spot pricing (approximate):**
- c7i.8xlarge (32 vCPU): ~$0.40-0.60/hour (~$0.10 per 15-min session)
- c7i.16xlarge (64 vCPU): ~$0.80-1.20/hour (~$0.20 per 15-min session)

**Always-on broker:**
- Run on existing VPS: $0/month additional
- Or AWS t4g.nano: ~$3/month

**Typical usage scenarios:**
- Light development (5 sessions/day): ~$5-6/month
- Active development (20 sessions/day): ~$20-25/month
- Heavy usage (8 hours/day): ~$35-50/month

---

## Initial AWS Setup

### 1. Install AWS CLI

```bash
brew install awscli
```

Verify installation:
```bash
aws --version
```

### 2. Configure AWS Credentials

If you already have an AWS account (e.g., for Glacier backups), create a new IAM user for EC2:

1. Go to AWS Console: https://console.aws.amazon.com/
2. Navigate to IAM → Users → Create user
3. Name: `compute-broker`
4. Attach policy: **AmazonEC2FullAccess**
5. Create access key for CLI usage
6. Save Access Key ID and Secret Access Key

Configure on your Mac:
```bash
aws configure
# AWS Access Key ID: <your-key-id>
# AWS Secret Access Key: <your-secret-key>
# Default region: us-east-1
# Default output format: json
```

Test configuration:
```bash
aws ec2 describe-instances
```

Should return empty instances list (or existing instances) without errors.

---

## Security Group Setup

Security groups act as firewalls for EC2 instances.

### Create Security Group

```bash
aws ec2 create-security-group \
    --group-name dserv-workers \
    --description "Security group for dserv compute workers"
```

Returns: `GroupId` like `sg-0123456789abcdef0` - **save this!**

### Get Your Public IP

```bash
curl ifconfig.me
```

Returns your current IP address (e.g., `123.45.67.89`)

### Add Firewall Rules

**Allow SSH from your IP (for debugging):**
```bash
aws ec2 authorize-security-group-ingress \
    --group-name dserv-workers \
    --protocol tcp \
    --port 22 \
    --cidr YOUR_IP/32
```

**Allow dserv communication on port 2560:**
```bash
aws ec2 authorize-security-group-ingress \
    --group-name dserv-workers \
    --protocol tcp \
    --port 2560 \
    --cidr 0.0.0.0/0
```

Note: `0.0.0.0/0` allows from anywhere. Restrict to your lab network CIDR if preferred.

### Retrieve Security Group ID

If you didn't save it earlier:
```bash
aws ec2 describe-security-groups \
    --group-names dserv-workers \
    --query 'SecurityGroups[0].GroupId' \
    --output text
```

---

## SSH Key Setup

AWS requires SSH key pairs for instance access.

### Create Key Pair

```bash
aws ec2 create-key-pair \
    --key-name dserv-workers \
    --query 'KeyMaterial' \
    --output text > ~/.ssh/dserv-workers.pem

chmod 400 ~/.ssh/dserv-workers.pem
```

This creates a private key stored locally at `~/.ssh/dserv-workers.pem`.

---

## Finding Ubuntu AMI

Find the latest Ubuntu 24.04 AMI for your region:

```bash
aws ec2 describe-images \
    --owners 099720109477 \
    --filters "Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*" \
    --query 'Images | sort_by(@, &CreationDate) | [-1].ImageId' \
    --output text
```

Returns: AMI ID like `ami-0c1f44f890950b53c`

---

## Launch Test Instance

### Launch 32-core Spot Instance

```bash
aws ec2 run-instances \
    --image-id ami-0c1f44f890950b53c \
    --instance-type c7i.8xlarge \
    --key-name dserv-workers \
    --security-group-ids sg-YOUR_GROUP_ID \
    --instance-market-options '{"MarketType":"spot"}' \
    --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=dserv-test-worker}]'
```

**Parameters explained:**
- `--image-id`: Ubuntu 24.04 base AMI
- `--instance-type`: c7i.8xlarge = 32 vCPU (use c7i.16xlarge for 64 vCPU)
- `--key-name`: SSH key pair created earlier
- `--security-group-ids`: Your security group ID
- `--instance-market-options`: Request spot instance (much cheaper)
- `--tag-specifications`: Tag for easy identification

### Get Instance Public IP

```bash
aws ec2 describe-instances \
    --filters "Name=tag:Name,Values=dserv-test-worker" "Name=instance-state-name,Values=running" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' \
    --output text
```

Returns: Public IP like `98.92.32.32`

### SSH to Instance

```bash
ssh -i ~/.ssh/dserv-workers.pem ubuntu@PUBLIC_IP
```

Verify CPU count:
```bash
nproc  # Should show 32
lscpu  # Detailed CPU info
```

---

## Create Custom AMI

After installing and configuring dserv on the test instance:

### Get Instance ID

```bash
INSTANCE_ID=$(aws ec2 describe-instances \
    --filters "Name=tag:Name,Values=dserv-test-worker" "Name=instance-state-name,Values=running" \
    --query 'Reservations[0].Instances[0].InstanceId' \
    --output text)
```

### Create AMI

```bash
aws ec2 create-image \
    --instance-id $INSTANCE_ID \
    --name "dserv-worker-v1" \
    --description "Ubuntu 24.04 with dserv stack for compute workers"
```

Returns: `ImageId` like `ami-0060337ebb6cf00ad` - **save this!**

### Check AMI Status

```bash
aws ec2 describe-images \
    --image-ids ami-0060337ebb6cf00ad \
    --query 'Images[0].State'
```

Status changes: `pending` → `available` (takes ~5 minutes)

### Terminate Test Instance

Once AMI is ready:
```bash
aws ec2 terminate-instances --instance-ids $INSTANCE_ID
```

---

## Broker Setup

The broker is a Python service that manages EC2 instances on demand.

### Install Dependencies (using uv)

```bash
mkdir compute-broker
cd compute-broker
uv init
uv add boto3
```

### Broker Code

See `broker.py` (full code below). Key configuration variables:

```python
LISTEN_PORT = 9000
IDLE_TIMEOUT = 900  # 15 minutes
AMI_ID = "ami-0060337ebb6cf00ad"  # Your custom AMI
INSTANCE_TYPE = "c7i.16xlarge"     # 64 cores (or c7i.8xlarge for 32)
SECURITY_GROUP = "sg-YOUR_GROUP_ID"
KEY_NAME = "dserv-workers"
```

### Run Broker

```bash
uv run broker.py
```

Output:
```
Broker listening on port 9000
```

---

## Client Integration

### Tcl Client Example

**Request worker:**
```tcl
set sock [socket $broker_host 9000]
puts $sock "GET_WORKER"
flush $sock
gets $sock worker_ip
close $sock

puts "Got worker at: $worker_ip"

# Now connect to worker for actual compute
set worker_sock [socket $worker_ip 2560]
# ... your dserv protocol ...
close $worker_sock
```

**Release worker when done:**
```tcl
set sock [socket $broker_host 9000]
puts $sock "RELEASE $worker_ip"
flush $sock
gets $sock response  ;# Should get "OK"
close $sock
```

### Python Client Example

```python
import socket

# Get worker
sock = socket.socket()
sock.connect(('broker_host', 9000))
sock.send(b'GET_WORKER\n')
worker_ip = sock.recv(1024).decode().strip()
sock.close()

print(f"Worker IP: {worker_ip}")

# ... do work on worker_ip:2560 ...

# Release worker
sock = socket.socket()
sock.connect(('broker_host', 9000))
sock.send(f'RELEASE {worker_ip}\n'.encode())
sock.close()
```

---

## Broker Protocol

The broker accepts TCP connections on port 9000 with simple text commands:

**GET_WORKER** (or empty string)
- Returns: Worker IP address as text
- Reuses existing idle worker if available (< 15 min since last use)
- Launches new spot instance if needed (~30-60 seconds)

**RELEASE <ip_address>**
- Immediately terminates the worker at specified IP
- Returns: "OK"
- Use when computation completes to avoid paying for idle time

---

## Useful AWS Commands

### List Running Instances

```bash
aws ec2 describe-instances \
    --filters "Name=instance-state-name,Values=running" \
    --query 'Reservations[*].Instances[*].[InstanceId,InstanceType,PublicIpAddress,Tags[?Key==`Name`].Value|[0]]' \
    --output table
```

### Terminate All dserv Workers

```bash
aws ec2 terminate-instances --instance-ids $(aws ec2 describe-instances \
    --filters "Name=tag:Name,Values=dserv-worker" "Name=instance-state-name,Values=running" \
    --query 'Reservations[].Instances[].InstanceId' \
    --output text)
```

### Check Spot Instance Requests

```bash
aws ec2 describe-spot-instance-requests \
    --filters "Name=state,Values=active,closed" \
    --query 'SpotInstanceRequests[*].[InstanceId,CreateTime,InstanceType]' \
    --output table
```

### List Your AMIs

```bash
aws ec2 describe-images --owners self
```

### Delete AMI

```bash
aws ec2 deregister-image --image-id ami-0060337ebb6cf00ad
```

---

## IP Address Confusion

AWS uses NAT (Network Address Translation):

**Private IP** (shown by `ip addr` on instance):
- Example: `172.31.95.52`
- Only accessible within AWS VPC
- Does NOT change if you stop/start instance
- Used for AWS-internal communication

**Public IP** (what you connect to):
- Example: `98.92.32.32`
- Changes every time instance is launched
- AWS NAT gateway translates public → private
- This is what broker returns to clients

The broker always returns the **public IP** via the AWS API.

---

## Cost Monitoring

### AWS Console Billing

https://console.aws.amazon.com/billing/home

- Today's charges appear next day
- Use "Cost Explorer" for detailed breakdown
- Check "Bills" for current month total

### Set Up Billing Alert

1. Billing Dashboard → Budgets
2. Create budget (e.g., alert if monthly cost > $50)
3. Enter email for notifications

### Estimate Current Session Costs

Based on instance type and runtime:
- c7i.8xlarge spot: ~$0.50/hour → ~$0.02/minute
- c7i.16xlarge spot: ~$1.00/hour → ~$0.04/minute

AMI storage: ~$0.05/month for typical 8GB snapshot

---

## Troubleshooting

### Instance Won't Start

Check spot availability:
```bash
aws ec2 describe-spot-price-history \
    --instance-types c7i.8xlarge \
    --start-time $(date -u +%Y-%m-%dT%H:%M:%S) \
    --product-descriptions "Linux/UNIX" \
    --query 'SpotPriceHistory[*].[AvailabilityZone,SpotPrice]' \
    --output table
```

### Can't SSH to Instance

1. Verify security group allows your IP:
```bash
aws ec2 describe-security-groups --group-names dserv-workers
```

2. Check instance is actually running:
```bash
aws ec2 describe-instances --instance-ids i-YOUR_ID
```

3. Verify you're using correct key:
```bash
ssh -i ~/.ssh/dserv-workers.pem ubuntu@PUBLIC_IP
```

### Worker Not Terminating

Check broker logs for errors. Manually terminate if needed:
```bash
aws ec2 terminate-instances --instance-ids i-WORKER_ID
```

### Orphaned Instances After Broker Restart

The broker discovers existing workers on startup, but if it can't find them:
```bash
# List all dserv workers
aws ec2 describe-instances \
    --filters "Name=tag:Name,Values=dserv-worker" "Name=instance-state-name,Values=running"

# Terminate them
aws ec2 terminate-instances --instance-ids i-ID1 i-ID2
```

---

## Production Deployment

### Deploy Broker to VPS

**Step 1: Prepare directory structure on VPS**
```bash
ssh your-vps

# Create and take ownership of broker directory
sudo mkdir -p /opt/compute-broker
sudo chown your-username:your-username /opt/compute-broker
```

**Step 2: Copy files from development machine**
```bash
# From your Mac
scp broker.py your-vps:/opt/compute-broker/
scp pyproject.toml your-vps:/opt/compute-broker/
scp .python-version your-vps:/opt/compute-broker/
```

**Step 3: Setup on VPS**
```bash
ssh your-vps
cd /opt/compute-broker

# Install uv if needed (user-local installation)
curl -LsSf https://astral.sh/uv/install.sh | sh

# Install dependencies
uv sync

# Configure AWS credentials (if not already done)
aws configure
# Or copy existing credentials:
# cp -r ~/.aws /opt/compute-broker/.aws
```

**Step 4: Open firewall port**
```bash
sudo ufw allow 9000/tcp
sudo ufw status
```

**Step 5: Test broker manually first**
```bash
cd /opt/compute-broker
uv run broker.py
# Verify it starts and listens on port 9000
# Test connection from client
# Ctrl-C to stop
```

**Step 6: Setup systemd service**

Create `/etc/systemd/system/compute-broker.service`:
```bash
sudo nano /etc/systemd/system/compute-broker.service
```

Add this content (replace `your-username` with actual username):
```ini
[Unit]
Description=dserv Compute Broker
After=network.target

[Service]
Type=simple
User=your-username
WorkingDirectory=/opt/compute-broker
ExecStart=/home/your-username/.cargo/bin/uv run broker.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

**Step 7: Enable and start service**
```bash
# Reload systemd to recognize new service
sudo systemctl daemon-reload

# Enable service to start on boot
sudo systemctl enable compute-broker

# Start service now
sudo systemctl start compute-broker

# Check status
sudo systemctl status compute-broker
```

**Step 8: Verify and monitor**

View real-time logs:
```bash
sudo journalctl -u compute-broker -f
```

Check service status:
```bash
sudo systemctl status compute-broker
```

**Service Management Commands:**
```bash
# Stop service
sudo systemctl stop compute-broker

# Restart service (after config changes)
sudo systemctl restart compute-broker

# Disable auto-start on boot
sudo systemctl disable compute-broker

# View recent logs
sudo journalctl -u compute-broker -n 100

# View logs since boot
sudo journalctl -u compute-broker -b
```

**Troubleshooting:**

If service fails to start:
```bash
# Check logs for errors
sudo journalctl -u compute-broker -xe

# Common issues:
# 1. Wrong path to uv - check with: which uv
# 2. AWS credentials not accessible - verify ~/.aws exists
# 3. Port 9000 already in use - check with: sudo lsof -i :9000
# 4. Permissions on /opt/compute-broker - should be owned by your user
```

**Update broker code:**
```bash
# Stop service
sudo systemctl stop compute-broker

# Update files
cd /opt/compute-broker
# Edit broker.py or copy new version

# Restart service
sudo systemctl start compute-broker

# Check it started successfully
sudo systemctl status compute-broker
```

---

## Full Broker Code

```python
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
AMI_ID = "ami-0060337ebb6cf00ad"
INSTANCE_TYPE = "c7i.16xlarge"  # 64 cores (or c7i.8xlarge for 32)
SECURITY_GROUP = "sg-YOUR_GROUP_ID"  # Replace with your security group ID
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
        """Launch a new spot instance and return its IP (synchronous)"""
        response = self.ec2.run_instances(
            ImageId=AMI_ID,
            InstanceType=INSTANCE_TYPE,
            KeyName=KEY_NAME,
            SecurityGroups=['dserv-workers'],
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
        print(f"Launched {instance_id}, waiting for IP...")
        
        # Wait for instance to be running and get IP
        ip = self.wait_for_instance(instance_id)
        
        self.workers[instance_id] = {
            'ip': ip,
            'last_used': time.time()
        }
        
        print(f"Worker {instance_id} ready at {ip}")
        return ip
    
    def wait_for_instance(self, instance_id, timeout=120):
        """Wait for instance to be running and return public IP"""
        start = time.time()
        while time.time() - start < timeout:
            response = self.ec2.describe_instances(InstanceIds=[instance_id])
            instance = response['Reservations'][0]['Instances'][0]
            
            state = instance['State']['Name']
            if state == 'running':
                ip = instance.get('PublicIpAddress')
                if ip:
                    return ip
            
            time.sleep(2)
        
        raise TimeoutError(f"Instance {instance_id} didn't start in {timeout}s")
    
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
```

---

## Next Steps / Enhancements

### Security Improvements
- Add authentication to broker protocol
- Restrict security group to lab network CIDR instead of 0.0.0.0/0
- Use IAM roles instead of access keys where possible
- Rotate AWS credentials periodically

### Monitoring & Logging
- Log all broker actions to file with timestamps
- Track cost per session
- Send alerts on errors or unusual activity
- Dashboard for active workers and usage stats

### Advanced Features
- Support multiple instance types (let client request 32 vs 64 cores)
- Worker pools (keep N workers warm during business hours)
- Graceful handling of spot interruptions (2-minute warning)
- Fallback to on-demand if spot unavailable
- Geographic region selection for lower latency

### Integration
- Add broker discovery via DNS/service discovery
- Integrate with existing dserv infrastructure seamlessly
- Create wrapper scripts for common workflows
- Build web UI for monitoring and manual control

---

## Summary

You now have:
- ✅ AWS EC2 configured with spot instances
- ✅ Custom AMI with dserv pre-installed
- ✅ Python broker managing worker lifecycle
- ✅ Tcl client integration
- ✅ ~70% cost savings via spot pricing
- ✅ On-demand 32-64 core compute in under 60 seconds

**Typical workflow:**
1. Client requests worker from broker
2. Broker launches or reuses spot instance
3. Client gets IP, connects directly to worker
4. Embarrassingly parallel workload runs at 100% across all cores
5. Client releases worker when done
6. Broker terminates instance (or keeps warm for 15 min)

**Cost:** Only pay for actual compute time used, approximately $0.10-0.20 per short session on 64-core instances.
