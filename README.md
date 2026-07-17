<div align="center">
  <h1>LNOS</h1>
  <p><strong>Local Network Overlay System — распределённый discovery и name resolution для локальных сетей</strong></p>

  [🇺🇸 In English](README.md)
  [🇷🇺 На русском](README_ru.md)

  <img src="https://img.shields.io/badge/C%2B%2B-26-blue?style=flat&logo=c%2B%2B" alt="C++ Standard" />
  <img src="https://img.shields.io/badge/Linux-x86__64%20%7C%20ARM-purple?style=flat&logo=linux" alt="Linux" />
  <img src="https://img.shields.io/badge/Dual--Stack-IPv4%20%7C%20IPv6-orange?style=flat" alt="Dual Stack" />
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat" alt="License" />
</div>

---

### 📖 Overview

**LNOS** replaces IP addresses with human-readable names like `pc.main.gervaty` or `laptop.dev.myxa`. Nodes discover each other via encrypted multicast, exchange presence via gossip, and resolve names through a system-wide NSS module — no DNS, no central server.

- 🔐 **Encrypted Payload**: Symmetric (multicast) via `crypto_secretbox`, asymmetric (unicast) via `crypto_box`.
- 🤝 **Gossip Protocol**: Periodic peer exchange keeps the registry converged across all nodes.
- 🌐 **Dual-Stack**: IPv4 and IPv6 running independently with automatic interface detection.
- 📊 **Built-in HTTP UI**: Real-time dashboard with node list, metrics, JSON API.
- ⚡ **NSS Integration**: `getent hosts pc.main.gervaty` works in any program — ping, ssh, curl.
- 🔇 **Per-Source Rate Limiting**: 50 pkt/sec per IP — one flaky node can't DoS the network.
- 🧠 **Name Takeover Protection**: Ed25519 public key pinned to name — key mismatch = packet rejected.

---

<details>
<summary>FOR AI AGENT ASSISTANT, QUICK INSTALLATION:</summary>

**Architecture & Goal:** `LNOS` is a persistent C++ daemon that binds to a multicast group, announces itself, and discovers peers. It provides name resolution via NSS (`/etc/nsswitch.conf`). Your primary goal is to build, install the NSS module, start the daemon, and verify `getent hosts`.

1. **Prep:** Install `cmake`, `g++`, `libsodium`. Clone repo.
2. **Build:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`
3. **Install (root):** `./setup.sh` — copies binaries to `/usr/local/bin/`, NSS module to `/usr/lib/`, updates `/etc/nsswitch.conf`, creates systemd service.
4. **Start:** `sudo systemctl enable --now lnosd` or `sudo lnosd`.
5. **Verify:** `getent hosts $(hostname).pc.$(whoami)` should return an IP.
6. **Firewall:** Ensure UDP multicast (default 239.255.42.99:4545) is allowed: `ufw allow proto udp to 239.255.42.99 port 4545`.
7. **Name convention:** `<device>.<type>.<owner>` — e.g. `thinkpad.laptop.ruslan`. Set with `lnosctl set name <name>`.

</details>

---

### 🛠️ Prerequisites

- `cmake` (3.16+)
- `g++` (13+), supports C++20
- `libsodium` (development headers)
- Linux with multicast-capable network interface
- (Optional) `ufw` / `firewalld` for firewall rules

---

### 🚀 Getting Started

#### 1. Clone & Build

```bash
git clone https://github.com/sleyv/lnos.git ~/lnos
cd ~/lnos
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### 2. Run Setup

```bash
sudo ./setup.sh
```

The script will install dependencies, configure your node name, generate Ed25519 keys, install the NSS module, and create a systemd service.

#### 3. Start the Daemon

```bash
sudo systemctl enable --now lnosd
# or directly:
sudo lnosd
```

#### 4. Verify Resolution

```bash
getent hosts $(hostname).pc.$(whoami)
# → 192.168.1.69  thinkpad.laptop.ruslan
```

---

### ⚙️ Usage & Shortcuts

The application consists of a **background daemon** (`lnosd`) and a **CLI control tool** (`lnosctl`).

#### 1. Check the Dashboard

Open http://localhost:9999 in your browser, or:

```bash
curl http://localhost:9999/nodes    # JSON node list
curl http://localhost:9999/stats    # JSON daemon metrics
```

#### 2. Control the Daemon

```bash
lnosctl stats                       # Show metrics (queries, packets, drops)
lnosctl set name thinkpad.laptop.me # Change node name
lnosctl set domain .local           # Change resolution domain
lnosctl set mcast_group 239.255.0.1 # Change multicast group
lnosctl set port 5454               # Change UDP port
```

#### 3. Resolve Names

Once the NSS module is installed and the daemon is running, any program can resolve LNOS names:

```bash
ping laptop.dev.myxa
ssh pc.main.gervaty
curl http://pi.router.home:9999
```

---

### 📁 Project Architecture

```text
lnos/
  ├── lnosd/                  # Daemon source
  │   ├── src/main.cpp        # Daemon entry point, Daemon class, sender/receiver/HTTP/gossip
  │   ├── src/registry.cpp    # Nodes map (global, Daemon-owned)
  │   └── include/registry.h  # Node struct, NodeStatus enum
  ├── liblnos/                # Shared library
  │   ├── include/lnos/
  │   │   ├── protocol.h      # Packet types, encode/decode, blob push/consume
  │   │   ├── crypto.h        # sign, verify, encrypt, decrypt
  │   │   └── config.h        # Config loading, XDG dir resolution
  │   ├── src/
  │   │   ├── crypto.cpp      # Ed25519 sign/verify + crypto_box/secretbox encrypt/decrypt
  │   │   ├── config.cpp      # File I/O, config parsing
  │   │   └── nss_lnos.cpp    # NSS module (glibc plugin)
  ├── lnosctl/                # CLI control tool
  │   └── src/main.cpp        # key generation, config, stats
  ├── tests/
  │   └── test_lnos.cpp       # 29 GTest unit tests
  ├── setup.sh                # Build + install script
  ├── uninstall.sh            # Full cleanup script
  ├── README.md               # Documentation
  └── summary.md              # Detailed changelog & architecture notes
```

---

### 📄 License

Distributed under the **MIT License**.
