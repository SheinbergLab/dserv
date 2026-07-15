# Makefile for dserv-agent

BINARY=dserv-agent
VERSION?=$(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
LDFLAGS=-ldflags "-X main.version=$(VERSION)"

# The extio provisioning script (served at /extio/setup) is go:embed'd, so its
# canonical source in wiznet-io/ must be mirrored into this package dir before
# any build. The committed copy lets `go build .` work without make; this keeps
# it fresh whenever the source changes.
PROVISION_SRC=../wiznet-io/provision.sh

# Build targets
.PHONY: all build clean install uninstall sync-provision

all: build

provision.sh: $(PROVISION_SRC)
	cp $(PROVISION_SRC) $@

sync-provision: provision.sh

build: provision.sh
	go build $(LDFLAGS) -o $(BINARY) .

# Cross-compile for common targets
# Note: CGO is required for sqlite3 support
build-linux-amd64: provision.sh
	CGO_ENABLED=1 GOOS=linux GOARCH=amd64 go build $(LDFLAGS) -o $(BINARY)-linux-amd64 .

build-linux-arm64: provision.sh
	CGO_ENABLED=1 GOOS=linux GOARCH=arm64 go build $(LDFLAGS) -o $(BINARY)-linux-arm64 .

build-linux-arm: provision.sh
	CGO_ENABLED=1 GOOS=linux GOARCH=arm GOARM=7 go build $(LDFLAGS) -o $(BINARY)-linux-armhf .

build-darwin-amd64: provision.sh
	CGO_ENABLED=1 GOOS=darwin GOARCH=amd64 go build $(LDFLAGS) -o $(BINARY)-darwin-amd64 .

build-darwin-arm64: provision.sh
	CGO_ENABLED=1 GOOS=darwin GOARCH=arm64 go build $(LDFLAGS) -o $(BINARY)-darwin-arm64 .

build-windows-arm64: provision.sh
	CGO_ENABLED=1 GOOS=windows GOARCH=arm64 go build $(LDFLAGS) -o $(BINARY)-windows-arm64 .

build-windows-amd64: provision.sh
	CGO_ENABLED=1 GOOS=windows GOARCH=amd64 go build $(LDFLAGS) -o $(BINARY)-windows-amd64 .
build-all: build-linux-amd64 build-linux-arm64 build-linux-arm build-darwin-amd64 build-darwin-arm64 build-windows-arm64 build-windows-amd64

# Install to system (requires root)
install: build
	install -m 755 $(BINARY) /usr/local/bin/$(BINARY)
	install -d /etc/dserv-agent
	install -m 600 env.example /etc/dserv-agent/env.example
	install -m 644 dserv-agent.service /etc/systemd/system/
	systemctl daemon-reload
	@echo ""
	@echo "Installation complete!"
	@echo ""
	@echo "To configure authentication:"
	@echo "  sudo cp /etc/dserv-agent/env.example /etc/dserv-agent/env"
	@echo "  sudo nano /etc/dserv-agent/env"
	@echo ""
	@echo "To start the service:"
	@echo "  sudo systemctl enable dserv-agent"
	@echo "  sudo systemctl start dserv-agent"

uninstall:
	systemctl stop dserv-agent || true
	systemctl disable dserv-agent || true
	rm -f /etc/systemd/system/dserv-agent.service
	rm -f /usr/local/bin/$(BINARY)
	systemctl daemon-reload
	@echo "Uninstalled. Config files in /etc/dserv-agent/ preserved."

clean:
	rm -f $(BINARY) $(BINARY)-*

# Development helpers
run:
	go run . -v

test:
	go test -v ./...

fmt:
	go fmt ./...

# Dependencies
deps:
	go mod download
	go mod tidy
