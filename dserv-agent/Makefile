# Makefile for dserv-agent

BINARY=dserv-agent
VERSION?=$(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
LDFLAGS=-ldflags "-X main.version=$(VERSION)"

# Build targets
.PHONY: all build clean install uninstall

all: build

build:
	go build $(LDFLAGS) -o $(BINARY) .

# Cross-compile for common targets
# Note: CGO is required for sqlite3 support
build-linux-amd64:
	CGO_ENABLED=1 GOOS=linux GOARCH=amd64 go build $(LDFLAGS) -o $(BINARY)-linux-amd64 .

build-linux-arm64:
	CGO_ENABLED=1 GOOS=linux GOARCH=arm64 go build $(LDFLAGS) -o $(BINARY)-linux-arm64 .

build-linux-arm:
	CGO_ENABLED=1 GOOS=linux GOARCH=arm GOARM=7 go build $(LDFLAGS) -o $(BINARY)-linux-armhf .

build-darwin-amd64:
	CGO_ENABLED=1 GOOS=darwin GOARCH=amd64 go build $(LDFLAGS) -o $(BINARY)-darwin-amd64 .

build-darwin-arm64:
	CGO_ENABLED=1 GOOS=darwin GOARCH=arm64 go build $(LDFLAGS) -o $(BINARY)-darwin-arm64 .

build-windows-arm64:
	CGO_ENABLED=1 GOOS=windows GOARCH=arm64 go build $(LDFLAGS) -o $(BINARY)-windows-arm64 .

build-windows-amd64:
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
