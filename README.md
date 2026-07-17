<div align="center">
  <h1>LNOS</h1>
  <p><strong>Local Network Overlay System — distributed discovery and name resolution for local networks</strong></p>

  [🇺🇸 In English](README.md)
  [🇷🇺 На русском](README_ru.md)

  <img src="https://img.shields.io/badge/C%2B%2B-20%2F23%2F26-blue?style=flat&logo=c%2B%2B" alt="C++" />
  <img src="https://img.shields.io/badge/Linux-x86__64%20%7C%20ARM-purple?style=flat&logo=linux" alt="Linux" />
  <img src="https://img.shields.io/badge/Dual--Stack-IPv4%20%7C%20IPv6-orange?style=flat" alt="Dual Stack" />
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat" alt="License" />
</div>

---

### 📖 Overview

**LNOS** replaces IP addresses with human-readable names like `laptop.dev.myxa`. Nodes discover each other via encrypted multicast, exchange presence via gossip, and resolve names through a system-wide NSS module — no DNS, no central server.

- 🔐 **Encrypted Payload**: Symmetric (multicast) via `crypto_secretbox`, asymmetric (unicast) via `crypto_box`.
- 🤝 **Gossip Protocol**: Periodic peer exchange keeps the registry converged across all nodes.
- 🌐 **Dual-Stack**: IPv4 and IPv6 running independently with automatic interface detection.
- 📊 **Built-in HTTP Dashboard**: Real-time UI with node list, metrics, JSON API.
- ⚡ **NSS Integration**: `getent hosts laptop.dev.myxa` works in any program — ping, ssh, curl.
- 🔇 **Per-Source Rate Limiting**: 50 pkt/sec per IP — one flaky node can't DoS the network.
- 🧠 **Name Takeover Protection**: Ed25519 public key pinned to name — key mismatch = packet rejected.

---

### 🛠️ Prerequisites

- `cmake` (3.16+)
- `g++` (13+), supports C++20
- `libsodium` (development headers)
- Linux with multicast-capable network interface
- (Optional) `ufw` / `firewalld` / `nftables` / `iptables` for firewall rules

---

### 🚀 Getting Started

#### 1. One-Click Install

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/sleyv/lnos/master/setup.sh)"
```

Downloads, builds, generates keys, and installs the daemon + NSS module — all in one command.

#### 2. Or Clone & Build Manually

```bash
git clone https://github.com/sleyv/lnos.git ~/lnos
cd ~/lnos
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Then run `./setup.sh` to install.

```bash
./setup.sh
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

### ⚙️ Usage

The application consists of a **background daemon** (`lnosd`) and a **CLI control tool** (`lnosctl`).

#### 1. Dashboard

Open http://localhost:9999 in your browser, or use curl:

```bash
curl http://localhost:9999/nodes    # JSON node list
curl http://localhost:9999/stats    # JSON daemon metrics
```

#### 2. Control the Daemon

```bash
lnosctl stats                       # Show metrics (queries, packets, drops)
lnosctl set name thinkpad.laptop.me # Change node name
lnosctl set http_port 8080          # Change HTTP dashboard port
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
  │   ├── src/main.cpp        # Daemon class, sender/receiver/HTTP/gossip
  │   ├── src/registry.cpp    # Nodes map
  │   └── include/registry.h  # Node struct
  ├── liblnos/                # Shared library
  │   ├── include/lnos/
  │   │   ├── protocol.h      # Packet encode/decode, blob ops
  │   │   ├── crypto.h        # sign, verify, encrypt, decrypt
  │   │   └── config.h        # Config loading
  │   ├── src/
  │   │   ├── crypto.cpp      # Ed25519 + crypto_box/secretbox
  │   │   ├── config.cpp      # File I/O, config parsing
  │   │   └── nss_lnos.cpp    # NSS module (glibc plugin)
  ├── lnosctl/                # CLI tool
  │   └── src/main.cpp        # Key generation, config, stats
  ├── tests/
  │   └── test_lnos.cpp       # 49 GTest unit tests
  ├── setup.sh                # Build + install/uninstall script
  ├── LICENSE                 # MIT License
  └── README.md               # This file
```

---

### 📄 License

Distributed under the **MIT License**.
