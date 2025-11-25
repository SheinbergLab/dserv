package main

import (
	"encoding/json"
	"fmt"
	"net"
	"sort"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"
)

const (
	MESH_DISCOVERY_PORT = 12346
	PEER_TIMEOUT_MS     = 30000
	CLEANUP_INTERVAL_MS = 10000
)

// MeshPeer represents a discovered dserv instance
type MeshPeer struct {
	ApplianceID  string            `json:"applianceId"`
	Name         string            `json:"name"`
	Status       string            `json:"status"`
	IPAddress    string            `json:"ipAddress"`
	WebPort      int               `json:"webPort"`
	IsLocal      bool              `json:"isLocal"`
	LastSeen     int64             `json:"lastSeen"`
	CustomFields map[string]string `json:"customFields"`
}

// MeshDiscovery handles UDP-based server discovery
type MeshDiscovery struct {
	mu       sync.RWMutex
	peers    map[string]MeshPeer
	program  *tea.Program
	stopChan chan struct{}
}

// Bubble Tea messages for discovery events
type msgPeerDiscovered struct {
	peer MeshPeer
}

type msgPeerLost struct {
	applianceID string
}

type msgPeerCleanup struct{}

func NewMeshDiscovery() *MeshDiscovery {
	return &MeshDiscovery{
		peers:    make(map[string]MeshPeer),
		stopChan: make(chan struct{}),
	}
}

func (m *MeshDiscovery) SetProgram(p *tea.Program) {
	m.program = p
}

func (m *MeshDiscovery) Start() error {
	addr, err := net.ResolveUDPAddr("udp", fmt.Sprintf(":%d", MESH_DISCOVERY_PORT))
	if err != nil {
		return fmt.Errorf("failed to resolve mesh discovery address: %w", err)
	}

	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return fmt.Errorf("failed to start mesh discovery: %w", err)
	}

	// Start peer cleanup timer
	go m.cleanupLoop()

	// Listen for heartbeats
	go m.listenLoop(conn)

	return nil
}

func (m *MeshDiscovery) Stop() {
	close(m.stopChan)
}

func (m *MeshDiscovery) cleanupLoop() {
	ticker := time.NewTicker(time.Duration(CLEANUP_INTERVAL_MS) * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-m.stopChan:
			return
		case <-ticker.C:
			m.cleanupExpiredPeers()
			if m.program != nil {
				m.program.Send(msgPeerCleanup{})
			}
		}
	}
}

func (m *MeshDiscovery) listenLoop(conn *net.UDPConn) {
	defer conn.Close()
	buffer := make([]byte, 1024)

	for {
		select {
		case <-m.stopChan:
			return
		default:
		}

		conn.SetReadDeadline(time.Now().Add(1 * time.Second))
		n, clientAddr, err := conn.ReadFromUDP(buffer)
		if err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				continue
			}
			continue
		}

		m.processHeartbeat(buffer[:n], clientAddr.IP.String())
	}
}

func (m *MeshDiscovery) processHeartbeat(data []byte, senderIP string) {
	var heartbeat struct {
		Type        string `json:"type"`
		ApplianceID string `json:"applianceId"`
		Data        struct {
			Name    string `json:"name"`
			Status  string `json:"status"`
			WebPort int    `json:"webPort"`
		} `json:"data"`
	}

	if err := json.Unmarshal(data, &heartbeat); err != nil {
		return
	}

	if heartbeat.Type != "heartbeat" {
		return
	}

	// Clean up IPv6-mapped IPv4 addresses
	cleanIP := senderIP
	if strings.HasPrefix(cleanIP, "::ffff:") {
		cleanIP = cleanIP[7:]
	}

	// Skip localhost variants
	if cleanIP == "127.0.0.1" || cleanIP == "localhost" {
		return
	}

	peer := MeshPeer{
		ApplianceID:  heartbeat.ApplianceID,
		Name:         heartbeat.Data.Name,
		Status:       heartbeat.Data.Status,
		IPAddress:    cleanIP,
		WebPort:      heartbeat.Data.WebPort,
		IsLocal:      false,
		LastSeen:     time.Now().UnixMilli(),
		CustomFields: make(map[string]string),
	}

	m.mu.Lock()
	_, exists := m.peers[heartbeat.ApplianceID]
	m.peers[heartbeat.ApplianceID] = peer
	m.mu.Unlock()

	// Notify program of new/updated peer
	if !exists && m.program != nil {
		m.program.Send(msgPeerDiscovered{peer: peer})
	}
}

func (m *MeshDiscovery) cleanupExpiredPeers() {
	now := time.Now().UnixMilli()
	var lostPeers []string

	m.mu.Lock()
	for id, peer := range m.peers {
		if now-peer.LastSeen > PEER_TIMEOUT_MS {
			delete(m.peers, id)
			lostPeers = append(lostPeers, id)
		}
	}
	m.mu.Unlock()

	// Notify program of lost peers
	if m.program != nil {
		for _, id := range lostPeers {
			m.program.Send(msgPeerLost{applianceID: id})
		}
	}
}

// GetAvailableHosts returns all known hosts (local + discovered)
func (m *MeshDiscovery) GetAvailableHosts(cmdPort int) []MeshPeer {
	var hosts []MeshPeer

	// Add localhost if available
	if isLocalhostAvailable(cmdPort) {
		hosts = append(hosts, MeshPeer{
			ApplianceID: "localhost",
			Name:        "localhost",
			Status:      "local",
			IPAddress:   "localhost",
			IsLocal:     true,
			LastSeen:    time.Now().UnixMilli(),
		})
	}

	// Add discovered mesh peers
	m.mu.RLock()
	for _, peer := range m.peers {
		hosts = append(hosts, peer)
	}
	m.mu.RUnlock()

	// Sort by name for consistent ordering
	sort.Slice(hosts, func(i, j int) bool {
		// Localhost always first
		if hosts[i].IsLocal {
			return true
		}
		if hosts[j].IsLocal {
			return false
		}
		return hosts[i].Name < hosts[j].Name
	})

	return hosts
}

// GetPeerCount returns number of discovered peers (excluding localhost)
func (m *MeshDiscovery) GetPeerCount() int {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return len(m.peers)
}

func isLocalhostAvailable(port int) bool {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("localhost:%d", port), 500*time.Millisecond)
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

// SelectHostInteractive prints available hosts and returns selected one
// Used for non-TUI mode (direct CLI selection)
func (m *MeshDiscovery) SelectHostInteractive(cmdPort int) (*MeshPeer, error) {
	hosts := m.GetAvailableHosts(cmdPort)

	if len(hosts) == 0 {
		return nil, fmt.Errorf("no hosts available")
	}

	if len(hosts) == 1 {
		fmt.Printf("Auto-connecting to %s (%s)\n", hosts[0].Name, hosts[0].IPAddress)
		return &hosts[0], nil
	}

	fmt.Println("Available hosts:")
	for i, host := range hosts {
		hostType := "Remote"
		if host.IsLocal {
			hostType = "Local"
		}
		fmt.Printf("  %d. %s (%s) [%s] - %s\n",
			i+1, host.Name, host.IPAddress, hostType, host.Status)
	}

	fmt.Print("Select host (1-", len(hosts), "): ")
	var selection int
	_, err := fmt.Scanf("%d", &selection)
	if err != nil || selection < 1 || selection > len(hosts) {
		return nil, fmt.Errorf("invalid selection")
	}

	return &hosts[selection-1], nil
}
