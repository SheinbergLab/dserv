package main

// Host discovery for dserv-term: a DNS-SD browse for `_dserv._tcp`, the
// record every dserv publishes through its OS mDNS responder (see
// modules/mdns and docs/discovery.md). Replaces the UDP heartbeat listener
// this file used to be, whose sender left core dserv in 2025-12.
//
// The browse runs for the life of the program: a dserv that starts after
// us shows up on the next :scan, and one that stops is dropped when its
// record is withdrawn (or after PEER_TIMEOUT if it just vanished).

import (
	"context"
	"fmt"
	"net"
	"sort"
	"strings"
	"sync"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/grandcat/zeroconf"
)

const (
	DSERV_SERVICE_TYPE = "_dserv._tcp"
	PEER_TIMEOUT_MS    = 120000 // a record the responder never withdrew
	CLEANUP_INTERVAL_MS = 10000
)

// MeshPeer represents a discovered dserv instance
type MeshPeer struct {
	ApplianceID  string            `json:"applianceId"` // instance name
	Name         string            `json:"name"`
	Status       string            `json:"status"`
	IPAddress    string            `json:"ipAddress"`
	WebPort      int               `json:"webPort"`
	CmdPort      int               `json:"cmdPort"`
	IsLocal      bool              `json:"isLocal"`
	LastSeen     int64             `json:"lastSeen"`
	CustomFields map[string]string `json:"customFields"` // the TXT record
}

// MeshDiscovery browses DNS-SD for dserv instances
type MeshDiscovery struct {
	mu       sync.RWMutex
	peers    map[string]MeshPeer
	program  *tea.Program
	stopChan chan struct{}
	cancel   context.CancelFunc
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
	resolver, err := zeroconf.NewResolver(nil)
	if err != nil {
		return fmt.Errorf("mDNS resolver: %w", err)
	}

	entries := make(chan *zeroconf.ServiceEntry, 16)
	ctx, cancel := context.WithCancel(context.Background())
	m.cancel = cancel

	if err := resolver.Browse(ctx, DSERV_SERVICE_TYPE, "local.", entries); err != nil {
		cancel()
		return fmt.Errorf("mDNS browse: %w", err)
	}

	go m.cleanupLoop()
	go m.browseLoop(entries)

	return nil
}

func (m *MeshDiscovery) Stop() {
	if m.cancel != nil {
		m.cancel()
	}
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

func (m *MeshDiscovery) browseLoop(entries <-chan *zeroconf.ServiceEntry) {
	for {
		select {
		case <-m.stopChan:
			return
		case entry, ok := <-entries:
			if !ok {
				return
			}
			m.processEntry(entry)
		}
	}
}

// parseTXT turns the record's "key=value" strings into a map.
func parseTXT(txt []string) map[string]string {
	fields := make(map[string]string, len(txt))
	for _, kv := range txt {
		if i := strings.IndexByte(kv, '='); i >= 0 {
			fields[kv[:i]] = kv[i+1:]
		} else if kv != "" {
			fields[kv] = ""
		}
	}
	return fields
}

func (m *MeshDiscovery) processEntry(entry *zeroconf.ServiceEntry) {
	// A withdrawn record arrives with TTL 0.
	if entry.TTL == 0 {
		m.mu.Lock()
		_, existed := m.peers[entry.Instance]
		delete(m.peers, entry.Instance)
		m.mu.Unlock()
		if existed && m.program != nil {
			m.program.Send(msgPeerLost{applianceID: entry.Instance})
		}
		return
	}

	// Prefer an IPv4 address; dserv's listeners are reached by v4 in
	// practice and the web port is advertised the same way.
	var ip string
	if len(entry.AddrIPv4) > 0 {
		ip = entry.AddrIPv4[0].String()
	} else if len(entry.AddrIPv6) > 0 {
		ip = entry.AddrIPv6[0].String()
	} else {
		return // not resolved yet; zeroconf re-sends once it has the A record
	}

	fields := parseTXT(entry.Text)
	webPort := 0
	fmt.Sscanf(fields["web"], "%d", &webPort)

	peer := MeshPeer{
		ApplianceID:  entry.Instance,
		Name:         entry.Instance,
		Status:       fields["wg"],
		IPAddress:    ip,
		WebPort:      webPort,
		CmdPort:      entry.Port,
		IsLocal:      isLocalAddress(ip),
		LastSeen:     time.Now().UnixMilli(),
		CustomFields: fields,
	}

	m.mu.Lock()
	_, exists := m.peers[entry.Instance]
	m.peers[entry.Instance] = peer
	m.mu.Unlock()

	if !exists && m.program != nil {
		m.program.Send(msgPeerDiscovered{peer: peer})
	}
}

// isLocalAddress reports whether ip belongs to one of this machine's
// interfaces, i.e. the advertised dserv is the one running here.
func isLocalAddress(ip string) bool {
	target := net.ParseIP(ip)
	if target == nil {
		return false
	}
	if target.IsLoopback() {
		return true
	}
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return false
	}
	for _, a := range addrs {
		if ipn, ok := a.(*net.IPNet); ok && ipn.IP.Equal(target) {
			return true
		}
	}
	return false
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

// GetAvailableHosts returns all known hosts (local + discovered). cmdPort
// is the port to probe localhost on when no advertisement names it --
// a dserv built without the mdns module still answers on 2560.
func (m *MeshDiscovery) GetAvailableHosts(cmdPort int) []MeshPeer {
	var hosts []MeshPeer

	m.mu.RLock()
	haveLocal := false
	for _, peer := range m.peers {
		hosts = append(hosts, peer)
		if peer.IsLocal {
			haveLocal = true
		}
	}
	m.mu.RUnlock()

	// Add localhost if it answers and nothing advertised it
	if !haveLocal && isLocalhostAvailable(cmdPort) {
		hosts = append(hosts, MeshPeer{
			ApplianceID: "localhost",
			Name:        "localhost",
			Status:      "local",
			IPAddress:   "localhost",
			CmdPort:     cmdPort,
			IsLocal:     true,
			LastSeen:    time.Now().UnixMilli(),
		})
	}

	// Sort by name for consistent ordering
	sort.Slice(hosts, func(i, j int) bool {
		// Localhost always first
		if hosts[i].IsLocal != hosts[j].IsLocal {
			return hosts[i].IsLocal
		}
		return hosts[i].Name < hosts[j].Name
	})

	return hosts
}

// GetPeerCount returns number of discovered peers
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
