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

### 📖 Описание

**LNOS** заменяет IP-адреса человекочитаемыми именами вроде `pc.main.gervaty` или `laptop.dev.myxa`. Узлы находят друг друга через зашифрованный multicast, обмениваются реестром через gossip и резолвят имена через системный NSS-модуль — без DNS и центрального сервера.

- 🔐 **Шифрование**: Symmetric (multicast) через `crypto_secretbox`, asymmetric (unicast) через `crypto_box`.
- 🤝 **Gossip**: Периодический обмен реестром между пирами — конвергенция без потери узлов.
- 🌐 **Dual-Stack**: IPv4 и IPv6 работают независимо, автоопределение интерфейса.
- 📊 **Встроенный HTTP UI**: Дашборд с узлами, метриками и JSON API.
- ⚡ **NSS-интеграция**: `getent hosts pc.main.gervaty` работает в любых программах.
- 🔇 **Rate Limiting**: 50 пакетов/сек с одного IP — один сбойный узел не положит сеть.
- 🧠 **Защита имени**: Публичный ключ Ed25519 привязан к имени — несовпадение ключа = пакет отклонён.

---

<details>
<summary>ДЛЯ AI АГЕНТА, БЫСТРАЯ УСТАНОВКА:</summary>

**Архитектура:** `LNOS` — C++ демон, который слушает multicast группу, анонсирует себя и обнаруживает пиров. Резолвинг имён через NSS (`/etc/nsswitch.conf`).

1. **Зависимости:** `cmake`, `g++`, `libsodium`. Клонировать репозиторий.
2. **Сборка:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`
3. **Установка (root):** `./setup.sh` — копирует бинарники в `/usr/local/bin/`, NSS-модуль в `/usr/lib/`, правит `/etc/nsswitch.conf`, создаёт systemd-сервис.
4. **Запуск:** `sudo systemctl enable --now lnosd` или `sudo lnosd`.
5. **Проверка:** `getent hosts $(hostname).pc.$(whoami)` — должен вернуть IP.
6. **Фаервол:** Разрешить UDP multicast (дефолт 239.255.42.99:4545): `ufw allow proto udp to 239.255.42.99 port 4545`.
7. **Имя узла:** `<device>.<type>.<owner>`, например `thinkpad.laptop.ruslan`. Задаётся через `lnosctl set name <name>`.

</details>

---

### 🛠️ Требования

- `cmake` (3.16+)
- `g++` (13+), поддержка C++20
- `libsodium` (заголовочные файлы)
- Linux с multicast-совместимым сетевым интерфейсом
- (Опционально) `ufw` / `firewalld` для правил фаервола

---

### 🚀 Быстрый старт

<table align="center"><tr><td>
<h3 align="center">⚡ Установка в одну команду</h3>

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/sleyv/lnos/master/scripts/install.sh)"
```

Скачивает репозиторий, собирает, генерирует ключи, выбирает имя узла, устанавливает демон + NSS-модуль и создаёт systemd-сервис — всё одной командой.

</td></tr></table>

#### 1. Клонирование и сборка (или используй однострочник выше)

```bash
git clone https://github.com/sleyv/lnos.git ~/lnos
cd ~/lnos
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### 2. Запуск установки

```bash
./setup.sh
```

Скрипт установит зависимости, настроит имя узла, сгенерирует Ed25519 ключи, установит NSS-модуль и создаст systemd-сервис.

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

#### 1. Откройте дашборд

Перейдите на http://localhost:9999 или используйте curl:

```bash
curl http://localhost:9999/nodes    # JSON список узлов
curl http://localhost:9999/stats    # JSON метрики демона
```

#### 2. Управление демоном

```bash
lnosctl stats                       # Метрики: запросы, пакеты, дропы
lnosctl set name thinkpad.laptop.me # Сменить имя узла
lnosctl set domain .local           # Сменить домен резолвинга
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
  │   ├── src/main.cpp        # Точка входа, Daemon class, sender/receiver/HTTP/gossip
  │   ├── src/registry.cpp    # Карта узлов (глобальная, во владении Daemon)
  │   └── include/registry.h  # Node struct, NodeStatus enum
  ├── liblnos/                # Общая библиотека
  │   ├── include/lnos/
  │   │   ├── protocol.h      # Типы пакетов, encode/decode, blob push/consume
  │   │   ├── crypto.h        # sign, verify, encrypt, decrypt
  │   │   └── config.h        # Загрузка конфига, XDG dir resolution
  │   ├── src/
  │   │   ├── crypto.cpp      # Ed25519 sign/verify + crypto_box/secretbox encrypt/decrypt
  │   │   ├── config.cpp      # File I/O, парсинг конфига
  │   │   └── nss_lnos.cpp    # NSS-модуль (glibc plugin)
  ├── lnosctl/                # CLI-утилита управления
  │   └── src/main.cpp        # Генерация ключей, конфиг, статистика
  ├── tests/
  │   └── test_lnos.cpp       # 29 GTest unit-тестов
  ├── setup.sh                # Скрипт сборки и установки
  ├── uninstall.sh            # Скрипт полного удаления
  ├── README.md               # Документация (английский)
  ├── README_ru.md            # Документация (русский)
  └── summary.md              # Детальный список изменений и архитектура
```

---

### 📄 Лицензия

Распространяется под лицензией **MIT**.
