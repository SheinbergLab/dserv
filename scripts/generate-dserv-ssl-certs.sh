#!/bin/bash
# generate-dserv-ssl-certs.sh
# Generates self-signed SSL certificates for dserv WebSocket HTTPS/WSS support

set -e

# Configuration
CERT_DIR="/usr/local/dserv/ssl"
CERT_FILE="${CERT_DIR}/cert.pem"
KEY_FILE="${CERT_DIR}/key.pem"
DAYS_VALID=365

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================="
echo "dserv SSL Certificate Generator"
echo "=================================="
echo ""

# Check if running as root/sudo
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: This script must be run with sudo${NC}"
    echo "Usage: sudo $0 [hostname_or_ip]"
    exit 1
fi

# Get hostname/IP from argument or auto-detect
if [ -n "$1" ]; then
    HOST_IDENTIFIER="$1"
    echo "Using provided identifier: $HOST_IDENTIFIER"
else
    # Try to auto-detect primary IP
    if command -v hostname &> /dev/null; then
        HOSTNAME=$(hostname)
        echo "Auto-detected hostname: $HOSTNAME"
        
        # Try to get primary IP
        if command -v ipconfig &> /dev/null; then
            # macOS
            PRIMARY_IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo "")
        else
            # Linux
            PRIMARY_IP=$(hostname -I | awk '{print $1}' 2>/dev/null || echo "")
        fi
        
        if [ -n "$PRIMARY_IP" ]; then
            echo "Auto-detected IP: $PRIMARY_IP"
            HOST_IDENTIFIER="$PRIMARY_IP"
        else
            HOST_IDENTIFIER="$HOSTNAME"
        fi
    else
        HOST_IDENTIFIER="localhost"
        echo "Could not auto-detect, using: localhost"
    fi
fi

echo ""
echo "This will create a self-signed SSL certificate for:"
echo "  - ${HOST_IDENTIFIER}"
echo "  - localhost"
echo "  - *.local (mDNS)"
echo ""
echo -e "${YELLOW}Note: Browsers will show a security warning for self-signed certificates.${NC}"
echo "      This is normal and expected. Users can accept the warning to proceed."
echo ""

# Check if certificates already exist
if [ -f "$CERT_FILE" ] || [ -f "$KEY_FILE" ]; then
    echo -e "${YELLOW}Warning: Certificates already exist:${NC}"
    [ -f "$CERT_FILE" ] && echo "  - $CERT_FILE"
    [ -f "$KEY_FILE" ] && echo "  - $KEY_FILE"
    echo ""
    read -p "Overwrite existing certificates? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 0
    fi
    echo ""
fi

# Create directory if it doesn't exist
echo "Creating certificate directory: $CERT_DIR"
mkdir -p "$CERT_DIR"

# Build subjectAltName based on what we know
SUBJECT_ALT_NAME="DNS:localhost,DNS:*.local"

# Add hostname if different from localhost
if [ "$HOST_IDENTIFIER" != "localhost" ]; then
    # Check if it's an IP address or hostname
    if [[ $HOST_IDENTIFIER =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        SUBJECT_ALT_NAME="${SUBJECT_ALT_NAME},IP:${HOST_IDENTIFIER}"
    else
        SUBJECT_ALT_NAME="${SUBJECT_ALT_NAME},DNS:${HOST_IDENTIFIER}"
    fi
fi

echo "Generating SSL certificate..."
echo "  Subject: CN=${HOST_IDENTIFIER}"
echo "  SAN: ${SUBJECT_ALT_NAME}"
echo "  Valid for: ${DAYS_VALID} days"
echo ""

# Generate the certificate
openssl req -x509 -newkey rsa:4096 \
    -keyout "$KEY_FILE" \
    -out "$CERT_FILE" \
    -days "$DAYS_VALID" \
    -nodes \
    -subj "/CN=${HOST_IDENTIFIER}" \
    -addext "subjectAltName=${SUBJECT_ALT_NAME}" \
    2>/dev/null

# Set appropriate permissions
chmod 600 "$KEY_FILE"
chmod 644 "$CERT_FILE"

echo -e "${GREEN}✓ Certificates generated successfully!${NC}"
echo ""
echo "Certificate files:"
echo "  Certificate: $CERT_FILE"
echo "  Private Key: $KEY_FILE"
echo ""
echo "Certificate details:"
openssl x509 -in "$CERT_FILE" -noout -subject -dates 2>/dev/null
echo ""

# Platform-specific trust instructions
echo "=================================="
echo "Next Steps"
echo "=================================="
echo ""
echo "1. Restart dserv to load the new certificates"
echo ""

if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "2. [macOS] To trust this certificate system-wide (optional):"
    echo "   sudo security add-trusted-cert -d -r trustRoot \\"
    echo "     -k /Library/Keychains/System.keychain \\"
    echo "     $CERT_FILE"
    echo ""
    echo "3. Access dserv via HTTPS:"
    echo "   https://${HOST_IDENTIFIER}:2565/terminal"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "2. [Linux] To trust this certificate system-wide (optional):"
    echo "   sudo cp $CERT_FILE /usr/local/share/ca-certificates/dserv.crt"
    echo "   sudo update-ca-certificates"
    echo ""
    echo "3. Access dserv via HTTPS:"
    echo "   https://${HOST_IDENTIFIER}:2565/terminal"
else
    echo "2. Access dserv via HTTPS:"
    echo "   https://${HOST_IDENTIFIER}:2565/terminal"
fi

echo ""
echo -e "${YELLOW}Note: Your browser will show a security warning the first time.${NC}"
echo "      Click 'Advanced' and accept the certificate to proceed."
echo ""
