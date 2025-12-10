# dserv SSL/TLS Setup

## Overview

dserv supports both HTTP/WebSocket and HTTPS/secure WebSocket (WSS) connections. SSL/TLS certificates are **optional** but recommended for:

- **Safari compatibility** on remote connections (Safari blocks non-SSL WebSocket connections to remote servers)
- **Encrypted communication** for sensitive data
- **Production environments** where security is important

## Quick Start

dserv works out of the box **without** SSL certificates:
- HTTP server runs on port 2565
- WebSocket connections work in Chrome, Firefox, and Safari (localhost only)

To enable HTTPS/WSS support, simply generate certificates:

```bash
sudo /usr/local/dserv/scripts/generate-dserv-ssl-certs.sh
# Restart dserv
```

## Automatic Certificate Detection

dserv automatically detects SSL certificates at startup:

```
# Without certificates:
SSL certificates not found - starting HTTP/WS server on port 2565

# With certificates:
SSL certificates found - starting HTTPS/WSS server on port 2565
```

No configuration changes needed!

## Certificate Locations

dserv looks for certificates in:
```
/usr/local/dserv/ssl/cert.pem  - SSL certificate
/usr/local/dserv/ssl/key.pem   - Private key
```

## Generating Certificates

### Option 1: Use the Provided Script (Recommended)

```bash
# Auto-detect hostname/IP
sudo /usr/local/dserv/scripts/generate-dserv-ssl-certs.sh

# Or specify hostname/IP
sudo /usr/local/dserv/generate-dserv-ssl-certs.sh 192.168.1.100
sudo /usr/local/dserv/generate-dserv-ssl-certs.sh myserver.local

# Restart dserv
sudo systemctl restart dserv  # Linux
```

### Option 2: Manual Generation

```bash
sudo mkdir -p /usr/local/dserv/ssl

sudo openssl req -x509 -newkey rsa:4096 \
  -keyout /usr/local/dserv/ssl/key.pem \
  -out /usr/local/dserv/ssl/cert.pem \
  -days 365 -nodes \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,DNS:*.local,IP:192.168.1.100"

sudo chmod 600 /usr/local/dserv/ssl/key.pem
sudo chmod 644 /usr/local/dserv/ssl/cert.pem
```

Replace `192.168.1.100` with your server's IP address.

## Browser Certificate Warnings

Self-signed certificates will trigger browser security warnings. This is **normal and expected**.

### Accepting the Certificate

**Chrome/Edge:**
1. Click "Advanced"
2. Click "Proceed to [hostname] (unsafe)"

**Firefox:**
1. Click "Advanced"
2. Click "Accept the Risk and Continue"

**Safari:**
1. Click "Show Details"
2. Click "visit this website"

This needs to be done **once per browser/device**.

## Trusting the Certificate System-Wide (Optional)

To avoid the browser warning on every device:

### macOS
```bash
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain \
  /usr/local/dserv/ssl/cert.pem
```

### Linux (Ubuntu/Debian)
```bash
sudo cp /usr/local/dserv/ssl/cert.pem /usr/local/share/ca-certificates/dserv.crt
sudo update-ca-certificates
```

### Linux (RHEL/CentOS)
```bash
sudo cp /usr/local/dserv/ssl/cert.pem /etc/pki/ca-trust/source/anchors/
sudo update-ca-trust
```

## Production Use with Let's Encrypt

For production deployments with a public domain name, use Let's Encrypt for trusted certificates:

```bash
# Install certbot
sudo apt-get install certbot  # Ubuntu/Debian
# or
brew install certbot  # macOS

# Get certificate (requires domain name and port 80)
sudo certbot certonly --standalone -d yourdomain.com

# Link to dserv location
sudo mkdir -p /usr/local/dserv/ssl
sudo ln -sf /etc/letsencrypt/live/yourdomain.com/fullchain.pem /usr/local/dserv/ssl/cert.pem
sudo ln -sf /etc/letsencrypt/live/yourdomain.com/privkey.pem /usr/local/dserv/ssl/key.pem

# Restart dserv
sudo systemctl restart dserv
```

Let's Encrypt certificates are:
- **Free**
- **Trusted by all browsers** (no warnings)
- **Auto-renewable** (90-day validity)

## Troubleshooting

### "SSL certificates not found"
- Check files exist: `ls -l /usr/local/dserv/ssl/`
- Check permissions: `sudo chmod 644 /usr/local/dserv/ssl/cert.pem && sudo chmod 600 /usr/local/dserv/ssl/key.pem`

### "Connection refused" in browser
- Ensure using `https://` not `http://`
- Check firewall: `sudo ufw allow 2565/tcp`
- Verify dserv is running: `ps aux | grep dserv`

### Safari won't connect remotely even with SSL
- Ensure you're using `https://` URL
- Accept the certificate warning
- Clear Safari cache: Preferences → Privacy → Manage Website Data

### Certificate expired
Self-signed certificates expire after 365 days (1 year). Regenerate:
```bash
sudo ./generate-dserv-ssl-certs.sh
sudo systemctl restart dserv
```

## Security Considerations

### Self-Signed Certificates
- ✓ Good for: Lab/research environments, local networks, development
- ✓ Encrypts traffic
- ✗ Browser warnings on first use
- ✗ No identity verification

### Let's Encrypt Certificates  
- ✓ Trusted by all browsers
- ✓ Free and automated
- ✓ Identity verification
- ✗ Requires public domain name
- ✗ Requires port 80 access for verification

### For Lab/Research Use
Self-signed certificates are perfectly appropriate. The browser warning is just informing users that the certificate is self-signed, not that there's an actual security problem.

## Certificate Validity

Generated certificates are valid for **365 days** (1 year). After expiration:
1. Regenerate certificates: `sudo ./generate-dserv-ssl-certs.sh`
2. Restart dserv
3. Re-accept in browsers if needed

## Disabling SSL

To disable SSL and return to HTTP-only:

```bash
sudo rm /usr/local/dserv/ssl/cert.pem /usr/local/dserv/ssl/key.pem
sudo systemctl restart dserv
```

dserv will automatically fall back to HTTP mode.

## Technical Details

- **Certificate Type:** X.509 self-signed
- **Key Type:** RSA 4096-bit
- **Validity:** 365 days
- **Protocol:** TLS 1.2+
- **Cipher Suites:** Modern ciphers via OpenSSL

dserv uses uWebSockets with OpenSSL for SSL/TLS support.
