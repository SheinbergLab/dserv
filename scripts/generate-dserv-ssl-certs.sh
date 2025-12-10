#!/bin/bash
# generate-dserv-ssl-certs.sh
# Generates CA and server certificates for dserv WebSocket HTTPS/WSS support

set -e

# Configuration
CERT_DIR="../ssl"
CA_CERT_FILE="${CERT_DIR}/ca.pem"
CA_KEY_FILE="${CERT_DIR}/ca-key.pem"
CERT_FILE="${CERT_DIR}/cert.pem"
KEY_FILE="${CERT_DIR}/key.pem"
SERVER_CSR="${CERT_DIR}/server.csr"
CA_DAYS_VALID=3650  # 10 years for CA
SERVER_DAYS_VALID=365  # 1 year for server cert

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=================================="
echo "dserv SSL Certificate Generator"
echo "=================================="
echo ""

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

# Check if CA already exists
CA_EXISTS=false
if [ -f "$CA_CERT_FILE" ] && [ -f "$CA_KEY_FILE" ]; then
    CA_EXISTS=true
    echo -e "${BLUE}Found existing CA certificate${NC}"
    openssl x509 -in "$CA_CERT_FILE" -noout -subject -dates 2>/dev/null
    echo ""
    read -p "Use existing CA? (Y/n): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Nn]$ ]]; then
        CA_EXISTS=false
        echo "Will create new CA..."
    else
        echo "Using existing CA to sign server certificate"
    fi
    echo ""
fi

# Check if server certificates already exist
if [ -f "$CERT_FILE" ] || [ -f "$KEY_FILE" ]; then
    echo -e "${YELLOW}Warning: Server certificates already exist:${NC}"
    [ -f "$CERT_FILE" ] && echo "  - $CERT_FILE"
    [ -f "$KEY_FILE" ] && echo "  - $KEY_FILE"
    echo ""
    read -p "Overwrite existing server certificates? (y/N): " -n 1 -r
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

# Create CA if needed
if [ "$CA_EXISTS" = false ]; then
    echo -e "${GREEN}Creating Certificate Authority (CA)...${NC}"
    echo "  This CA will be used to sign server certificates"
    echo "  Valid for: ${CA_DAYS_VALID} days (10 years)"
    echo ""
    
    openssl req -x509 -new -nodes \
        -keyout "$CA_KEY_FILE" \
        -out "$CA_CERT_FILE" \
        -days "$CA_DAYS_VALID" \
        -newkey rsa:4096 \
        -subj "/CN=Brown Neuroscience Lab CA/O=Brown University/OU=Neuroscience" \
        2>/dev/null
    
    chmod 600 "$CA_KEY_FILE"
    chmod 644 "$CA_CERT_FILE"
    
    echo -e "${GREEN}✓ CA created successfully${NC}"
    echo ""
fi

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

echo -e "${GREEN}Generating server certificate...${NC}"
echo "  Subject: CN=${HOST_IDENTIFIER}"
echo "  SAN: ${SUBJECT_ALT_NAME}"
echo "  Valid for: ${SERVER_DAYS_VALID} days"
echo ""

# Create server certificate request
openssl req -new -nodes \
    -keyout "$KEY_FILE" \
    -out "$SERVER_CSR" \
    -newkey rsa:4096 \
    -subj "/CN=${HOST_IDENTIFIER}" \
    2>/dev/null

# Create extensions file for SAN
cat > "${CERT_DIR}/server_ext.cnf" << EOF
subjectAltName = ${SUBJECT_ALT_NAME}
extendedKeyUsage = serverAuth
EOF

# Sign server certificate with CA
openssl x509 -req \
    -in "$SERVER_CSR" \
    -CA "$CA_CERT_FILE" \
    -CAkey "$CA_KEY_FILE" \
    -CAcreateserial \
    -out "$CERT_FILE" \
    -days "$SERVER_DAYS_VALID" \
    -extfile "${CERT_DIR}/server_ext.cnf" \
    2>/dev/null

# Clean up temporary files
rm -f "$SERVER_CSR" "${CERT_DIR}/server_ext.cnf" "${CERT_DIR}/ca.srl"

# Set appropriate permissions
chmod 600 "$KEY_FILE"
chmod 644 "$CERT_FILE"

echo -e "${GREEN}✓ Server certificate generated successfully!${NC}"
echo ""
echo "Certificate files:"
echo "  CA Certificate:     $CA_CERT_FILE  ${YELLOW}(distribute to users)${NC}"
echo "  CA Private Key:     $CA_KEY_FILE  ${RED}(keep secret!)${NC}"
echo "  Server Certificate: $CERT_FILE"
echo "  Server Private Key: $KEY_FILE"
echo ""
echo "Server certificate details:"
openssl x509 -in "$CERT_FILE" -noout -subject -issuer -dates 2>/dev/null
echo ""

# After generating/detecting CA
if [ -d "../www" ]; then
    echo "Copying CA certificate to web directory..."
    cp "$CA_CERT_FILE" "../www/lab-ca.pem"
    chmod 644 "../www/lab-ca.pem"
fi

# Platform-specific trust instructions
echo "=================================="
echo "Next Steps"
echo "=================================="
echo ""
echo "1. Copy CA certificate to your web directory for download:"
echo "   cp $CA_CERT_FILE /path/to/www/lab-ca.pem"
echo ""
echo "2. Restart dserv to load the new certificates"
echo ""
echo -e "${BLUE}For Users - One-Time Setup:${NC}"
echo "Download and install lab-ca.pem:"

if [[ "$OSTYPE" == "darwin"* ]]; then
    echo ""
    echo "  macOS:"
    echo "    • Double-click lab-ca.pem"
    echo "    • Add to Keychain, set to 'Always Trust'"
    echo "  Or command line:"
    echo "    sudo security add-trusted-cert -d -r trustRoot \\"
    echo "      -k /Library/Keychains/System.keychain lab-ca.pem"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo ""
    echo "  Linux:"
    echo "    sudo cp lab-ca.pem /usr/local/share/ca-certificates/lab-ca.crt"
    echo "    sudo update-ca-certificates"
fi

echo ""
echo "  iPhone/iPad:"
echo "    • Download lab-ca.pem from Safari"
echo "    • Settings → Profile Downloaded → Install"
echo "    • Settings → General → About → Certificate Trust Settings"
echo "    • Enable full trust for the certificate"
echo ""
echo "  Windows:"
echo "    • Double-click lab-ca.pem"
echo "    • Install → Local Machine → Trusted Root Certification Authorities"
echo ""
echo "  Android:"
echo "    • Settings → Security → Install from storage"
echo "    • Select lab-ca.pem"
echo ""

echo "3. Access dserv via HTTPS:"
echo "   https://${HOST_IDENTIFIER}:2565/terminal"
echo ""
echo -e "${GREEN}Note: Once users install the CA certificate, all future server${NC}"
echo -e "${GREEN}certificates signed by this CA will be trusted automatically!${NC}"
echo ""
echo -e "${YELLOW}To renew server certificate (yearly):${NC}"
echo "  sudo $0 $HOST_IDENTIFIER"
echo ""