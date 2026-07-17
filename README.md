# LNOS — Local Network Overlay System

Lightweight distributed discovery and name resolution for local networks.

LNOS replaces IP addresses with human-readable hierarchical names (`device.type.owner`). Nodes discover each other via multicast UDP, exchange encrypted presence information, and resolve names through a local NSS module — no DNS, no central server.

```text
pc.main.gervaty    → 192.168.1.69
laptop.dev.myxa    → 192.168.1.238
pi.router.home     → 192.168.1.10
```

---

# Features

## Core

* UDP multicast discovery (IPv4 + IPv6 dual-stack)
* Automatic node registry with TTL-based cleanup
* Human-readable node naming (`device.type.owner`)
* NSS module — `getent hosts pc.main.gervaty` works system-wide
* Query/Response protocol for cache-miss resolution
* Configurable domain, multicast group, port (no hardcoded paths)

## Security

* Ed25519 packet signing and verification
* Name takeover prevention (C-2): public key pinned to name
* Payload encryption: symmetric (multicast) via `crypto_secretbox`, asymmetric (unicast) via `crypto_box`
* Private keys stored with restricted permissions (750/644)
* UNIX socket in `~/.config/lnos/` (not `/tmp`) — no symlink race

## Reliability

* Dual-stack IPv4 + IPv6 with independent sender/receiver threads
* Self-registration on startup — own name resolves even without multicast loopback
* Gossip protocol — periodic peer exchange keeps registry converged
* Per-source rate limiting (50 pkt/sec per IP)
* Atomic `owners.db` writes (tmp + rename)
* Graceful shutdown (SIGINT/SIGTERM)

## Observability

* HTTP dashboard on port 8080 — `curl localhost:8080/nodes`
* JSON API: `/nodes`, `/stats`
* HTML UI with auto-refresh and metrics cards
* Daemon metrics: queries resolved/failed, packets received/dropped/rejected
* `lnosctl stats` — statistics via UNIX socket

---

# Architecture

```text
┌──────────────┐         multicast UDP (encrypted)        ┌──────────────┐
│   Node A     │  ◄──────────────────────────────────────► │   Node B     │
│  notebook    │                                           │  archlinux   │
│  192.168.1.69│                                           │ 192.168.1.238│
└──────┬───────┘                                           └──────┬───────┘
       │                                                        │
       │  ┌──────────────────┐        ┌──────────────────┐      │
       │  │   HTTP :8080     │        │   HTTP :8080     │      │
       │  │   /nodes /stats  │        │   /nodes /stats  │      │
       │  └──────────────────┘        └──────────────────┘      │
       │                                                        │
       │  ┌──────────────────┐        ┌──────────────────┐      │
       └──│  NSS module      │        │  NSS module      │◄─────┘
          │  getent hosts    │        │  getent hosts    │
          └──────────────────┘        └──────────────────┘
```

Each node runs a single `Daemon` instance with:
- **Sender threads** — periodic encrypted announce on IPv4 and IPv6
- **Receiver threads** — decrypt, verify, store incoming packets
- **Query server** — UNIX socket for NSS resolution and `lnosctl stats`
- **Gossip thread** — periodic peer exchange (every 30s)
- **HTTP server** — REST API and web dashboard

---

# Concepts

## Node Identity

```
device.type.owner
```

Each node generates an Ed25519 keypair on first run. The public key is pinned to the name — if another node claims the same name with a different key, the packet is rejected.

## Lifecycle

Nodes announce themselves every 2 seconds. If a node stops announcing, it's marked offline after 15 seconds (TTL) and removed after 60 seconds (4×TTL). Gossip ensures the registry converges even if some announces are lost.

## Name Resolution

```
getent hosts pc.main.gervaty
       │
       ▼
  ┌─────────┐    miss    ┌──────────────┐
  │  local  │ ────────►  │  multicast   │
  │ registry│            │    Query     │
  │         │ ◄────────  │              │
  │   hit   │  Response  └──────────────┘
  └────┬────┘
       ▼
   IP address
```

The NSS module first checks the owner against `owners.db` (via mmap). If the owner is known, it queries the daemon's UNIX socket. If the daemon doesn't have the name cached, it sends a multicast Query — the target node responds with its IP.

---

# Quick Start

```bash
git clone https://github.com/sleyv/lnos.git
cd lnos
./setup.sh                           # build, install, configure
sudo systemctl enable --now lnosd    # start daemon
getent hosts $(hostname).pc.$(whoami) # verify resolution
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo ./build/lnosd                   # run daemon
./build/lnosctl stats                # check metrics
```

## CLI

| Command | Action |
|---------|--------|
| `lnosctl init` | Create config directory |
| `lnosctl generatekeys` | Generate Ed25519 keypair |
| `lnosctl set name <val>` | Set node name |
| `lnosctl set domain <val>` | Set resolution domain |
| `lnosctl set mcast_group <ip>` | Set IPv4 multicast group |
| `lnosctl set mcast_group_v6 <ip>` | Set IPv6 multicast group |
| `lnosctl set port <num>` | Set UDP port |
| `lnosctl stats` | Show daemon metrics |

## HTTP API (port 8080)

| Endpoint | Returns |
|----------|---------|
| `GET /` | HTML dashboard |
| `GET /nodes` | JSON list of known nodes |
| `GET /stats` | JSON daemon metrics |

---

# Security Model

1. **Identity:** Ed25519 keypair per node, generated on first run
2. **Authentication:** Every packet is signed; signature verified before processing
3. **Name pinning:** Public key is stored with the name — key mismatch = packet rejected
4. **Encryption:** Payload is encrypted using the sender's public key as symmetric key for multicast, or per-recipient `crypto_box` for unicast
5. **Key storage:** Config directory has `750` permissions, key files `644`

---

# Roadmap

- [x] Dual-stack IPv4/IPv6
- [x] Payload encryption
- [x] Gossip-based registry sync
- [x] Per-source rate limiting
- [x] HTTP dashboard and JSON API
- [x] Daemon metrics
- [ ] Service discovery (auto-registration)
- [ ] Direct node-to-node messaging
- [ ] Message routing
- [ ] Network topology visualization
- [ ] Prometheus metrics endpoint

---

# Status

LNOS is a working experimental system. All core features are implemented and tested: discovery, name resolution, encryption, gossip, metrics, and web UI. Builds and runs on Linux (systemd/OpenRC).

```
tests: 29/29 passed
build: C++26 (GCC 16), CMake, libsodium
license: MIT
