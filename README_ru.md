<div align="center">
  <h1>LNOS</h1>
  <p><strong>Local Network Overlay System — распределённый discovery и name resolution для локальных сетей</strong></p>

  [🇺🇸 In English](README.md)
  [🇷🇺 На русском](README_ru.md)

  <img src="https://img.shields.io/badge/C%2B%2B-20%2F23%2F26-blue?style=flat&logo=c%2B%2B" alt="C++" />
  <img src="https://img.shields.io/badge/Linux-x86__64%20%7C%20ARM-purple?style=flat&logo=linux" alt="Linux" />
  <img src="https://img.shields.io/badge/Dual--Stack-IPv4%20%7C%20IPv6-orange?style=flat" alt="Dual Stack" />
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat" alt="License" />
</div>

---

### 📖 Описание

**LNOS** заменяет IP-адреса человекочитаемыми именами вроде `laptop.dev.myxa`. Узлы находят друг друга через зашифрованный multicast, обмениваются реестром через gossip и резолвят имена через системный NSS-модуль — без DNS и центрального сервера.

- 🔐 **Шифрование**: симметричное (multicast) через `crypto_secretbox`, асимметричное (unicast) через `crypto_box`.
- 🤝 **Gossip**: периодический обмен реестром между пирами — конвергенция без потери узлов.
- 🌐 **Dual-Stack**: IPv4 и IPv6 работают независимо, автоопределение интерфейса.
- 📊 **Встроенный HTTP-дашборд**: веб-интерфейс с узлами, метриками и JSON API.
- ⚡ **NSS-интеграция**: `getent hosts laptop.dev.myxa` работает в любых программах — ping, ssh, curl.
- 🔇 **Rate Limiting**: 50 пакетов/сек с одного IP — один сбойный узел не положит сеть.
- 🧠 **Защита имени**: публичный ключ Ed25519 привязан к имени — несовпадение ключа = пакет отклонён.

---

### 🛠️ Требования

- `cmake` (3.16+)
- `g++` (13+), поддержка C++20
- `libsodium` (заголовочные файлы)
- Linux с multicast-совместимым сетевым интерфейсом
- (Опционально) `ufw` / `firewalld` / `nftables` / `iptables` для правил фаервола

---

### 🚀 Быстрый старт

#### 1. Установка в одну команду

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/sleyv/lnos/master/setup.sh)"
```

Скачивает, собирает, генерирует ключи и устанавливает демон + NSS-модуль — одной командой.

#### 2. Или клонирование и сборка вручную

```bash
git clone https://github.com/sleyv/lnos.git ~/lnos
cd ~/lnos
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Затем запустите `./setup.sh` для установки.

```bash
./setup.sh
```

Скрипт установит зависимости, настроит имя узла, сгенерирует Ed25519-ключи, установит NSS-модуль и создаст systemd-сервис.

#### 3. Запуск демона

```bash
sudo systemctl enable --now lnosd
# или напрямую:
sudo lnosd
```

#### 4. Проверка резолва

```bash
getent hosts $(hostname).pc.$(whoami)
# → 192.168.1.69  thinkpad.laptop.ruslan
```

---

### ⚙️ Использование

Проект состоит из **фонового демона** (`lnosd`) и **CLI-утилиты** (`lnosctl`).

#### 1. Дашборд

Откройте http://localhost:9999 в браузере или используйте curl:

```bash
curl http://localhost:9999/nodes    # JSON список узлов
curl http://localhost:9999/stats    # JSON метрики демона
```

#### 2. Управление демоном

```bash
lnosctl stats                       # Метрики: запросы, пакеты, дропы
lnosctl set name thinkpad.laptop.me # Сменить имя узла
lnosctl set http_port 8080          # Сменить порт HTTP-дашборда
lnosctl set mcast_group 239.255.0.1 # Сменить multicast группу
lnosctl set port 5454               # Сменить порт
```

#### 3. Резолвинг имён

После установки NSS-модуля любая программа может резолвить LNOS-имена:

```bash
ping laptop.dev.myxa
ssh pc.main.gervaty
curl http://pi.router.home:9999
```

---

### 📁 Архитектура проекта

```text
lnos/
  ├── lnosd/                  # Исходный код демона
  │   ├── src/main.cpp        # Daemon class, sender/receiver/HTTP/gossip
  │   ├── src/registry.cpp    # Карта узлов
  │   └── include/registry.h  # Node struct
  ├── liblnos/                # Общая библиотека
  │   ├── include/lnos/
  │   │   ├── protocol.h      # Encode/decode пакетов, blob-операции
  │   │   ├── crypto.h        # sign, verify, encrypt, decrypt
  │   │   └── config.h        # Загрузка конфига
  │   ├── src/
  │   │   ├── crypto.cpp      # Ed25519 + crypto_box/secretbox
  │   │   ├── config.cpp      # File I/O, парсинг конфига
  │   │   └── nss_lnos.cpp    # NSS-модуль (glibc plugin)
  ├── lnosctl/                # CLI-утилита
  │   └── src/main.cpp        # Генерация ключей, конфиг, статистика
  ├── tests/
  │   └── test_lnos.cpp       # 49 GTest unit-тестов
  ├── setup.sh                # Скрипт сборки и установки/удаления
  ├── LICENSE                 # MIT License
  └── README.md               # Этот файл
```

---

### 📄 Лицензия

Распространяется под лицензией **MIT**.
