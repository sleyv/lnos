# LNOS — Local Network Overlay System

## Репозиторий
- **URL:** https://github.com/sleyv/lnos (форк Teskum-Researches/lnos)
- **Ветка:** master

## Язык и сборка
- **C++26** (GCC 16.1.1), fallback C++23 → C++20. В CMake автоматический выбор стандарта.
- **Зависимости:** libsodium (Ed25519 подпись/верификация)
- **Сборка без интернета:** `cmake -DBUILD_TESTING=OFF` (GTest не подтягивается)
- **Артефакты:** `lnosd`, `lnosctl`, `libnss_lnos.so.2`

## Новые фичи (добавлено в форке)

### C++26 с fallback
CMake проверяет поддержку `-std=c++26` → `-std=c++23` → `-std=c++20`. Гарантированно собирается на GCC 14+ и новее.

### Dual-Stack IPv4/IPv6
Демон через `getifaddrs()` определяет активный сетевой интерфейс (первый non-loopback UP/RUNNING) и запускает независимые приёмники/передатчики для IPv4 и IPv6. На IPv6 сокет установлен `IPV6_V6ONLY` — иначе он перехватывает IPv4 трафик и bind падает с `EADDRINUSE`.

### Привязка multicast к интерфейсу
`IP_MULTICAST_IF` (по IPv4-адресу) и `IPV6_MULTICAST_IF` (по ifindex) при отправке Announce и Query. Пакеты уходят через обнаруженный интерфейс, а не через дефолтный маршрут.

### Self-registration узла
Узел регистрирует себя в `nodes` map при старте, не полагаясь на multicast loopback (который не работает на некоторых WiFi-драйверах). Резолвинг собственного имени работает всегда.

### NSS-модуль (libnss_lnos.so.2)
Системная интеграция: `getaddrinfo("device.type.owner")` работает в любых программах (ssh, ping, curl, getent). Модуль:
1. Проверяет TLD blacklist — если имя заканчивается на известный TLD (`.com`, `.org`, `.ru` и т.д.) → сразу `NOT_FOUND`
2. Извлекает owner (последняя часть имени) и проверяет через mmap `owners.db`
3. Если owner в сети — идёт в UNIX-сокет демона и получает IP
4. Если нет — `NOT_FOUND` без единого syscall'а

Отключение TLD blacklist: `LNOS_SKIP_TLDS=0`.

### mmap-based owner check
NSS читает `/etc/lnos/owners.db` через mmap — проверка owner'а без единого syscall'а после mmap. Если owner есть в сети — идёт в UNIX-сокет. Если нет — сразу `NOT_FOUND`.

### TLD blacklist
~40 популярных TLD (`.com`, `.org`, `.net`, `.ru`, `.io`, `.app`, `.dev`, `.local`, `.localhost` и др.) — NSS скипает их сразу, без mmap и сокета. Отключается `LNOS_SKIP_TLDS=0`.

### Query/Response протокол
Два новых типа пакетов. Если демон не знает имя в локальном реестре — рассылает multicast Query. Целевой узел отвечает unicast Response с IP. После этого имя кэшируется в реестре.

### GTest + 12 unit-тестов
GTest через FetchContent. Проверяется encode/decode (все типы пакетов, пустые имена, дубли портов, лимиты), Ed25519 подпись/верификация (включая tamper), config dir resolution (включая XDG).

### Автоопределение config-директории (XDG)
`getConfigDir()`: `$XDG_CONFIG_HOME` → `~/.config/lnos` → `/etc/lnos`. `createConfig()` использует `std::filesystem::create_directories()` — не требует root. Все операции с конфигом работают для обычных пользователей.

### Всё конфигурируется (без хардкода)
`lnosctl set`: `domain`, `mcast_group`, `mcast_group_v6`, `port`, `name`. Дефолты: `.gervaty`, `239.255.42.99:4545`, `ff02::4299:4545`. Никаких зашитых путей, адресов, имён.

### Atomic owners.db
`owners.db` пишется через tmp + rename — crash-safe. Права выставляются до rename. Список owner'ов собирается из активных узлов в реестре.

### Graceful shutdown
`stopWithError()` атомарно выставляет `running = false`, все потоки завершаются чисто. SIGINT обрабатывается корректно.

### Big Endian протокол
Бинарный протокол использует сетевой порядок байт (Big Endian). Две машины с разным порядком байтов понимают друг друга.

### Self-registration узла
Узел сразу добавляет себя в реестр при старте, без ожидания multicast loopback.

## Security-фиксы

| ID | Проблема | Решение |
|----|----------|---------|
| C-1 | Приватные ключи world-readable (umask) | `createConfig()`: 750 на директорию, 644 на файлы |
| C-2 | Name takeover — подписи не проверялись при привязке имени | В `Node` добавлено `publicKey`. Несовпадение ключа → пакет отклонён |
| C-3 | `sodium_init()` не вызывался — краш или тихая некорректная подпись | `sodium_init()` с проверкой в `main()`. Ключи грузятся однократно через `static` |
| C-4 | OOM через nodes map — clean-up только ставил `Offline`, не удаляя | Мёртвые ноды удаляются через 4×TTL. Лимит — 1000 узлов |
| C-5 | UNIX-сокет в `/tmp` — symlink race (LPE) | Сокет в `~/.config/lnos/lnosd.sock` |
| H-3/H-4 | OOM через decode — `uint64_t` длина строки/сервисов | Лимит строки 1024 байта, сервисов 256 |
| H-5 | Single-threaded query server — блокировка accept | Каждый клиент в отдельном треде. Backlog 128 |

### Missing IP_MULTICAST_IF в sendMulticastQuery
Query-пакеты уходили через дефолтный маршрут. Добавлен `IP_MULTICAST_IF` / `IPV6_MULTICAST_IF`.

## UB Fixes & Code Quality

- `union PacketAs` → прямое хранение (UB с `std::string` + `std::vector`)
- `*(uint64_t*)packet.data` → `std::memcpy` (strict aliasing + bus error на ARM)
- `default: return false` в decode — неизвестный тип пакета не читает мусор
- `std::shared_mutex` вместо `std::mutex` — читатели не блокируют друг друга
- Мёртвый код удалён: дубли `signPacket()`, неиспользуемые `publicKey`/`coutMutex`, глобальная `myIp`

## Тесты
- **12/12 GTest** — все зелёные
- Покрытие: encode/decode всех типов пакетов, пустые имена, дубли портов, лимиты строк и сервисов, Ed25519 подпись/верификация (включая tamper), config dir resolution, XDG

## Развёртывание
- **setup.sh** — автоопределение пакетного менеджера (apt/pacman/dnf/zypper/emerge/apk), сборка, установка NSS + nsswitch.conf + systemd/OpenRC + firewall + seeding owners.db
- **uninstall.sh** — полный откат: остановка демона, удаление NSS, откат nsswitch.conf, очистка firewall, удаление конфигов и build
- **systemd:** `lnosd.service` с `Restart=on-failure`, `After=network-online.target`
- **Проверка root:** оба скрипта проверяют `is_root()`

## Известные ограничения
1. Multicast не роутится между L2-сегментами без PIM — все узлы в одной подсети/VPN
2. Payload открытый (только подпись Ed25519, без шифрования)
3. Защита от DDoS: лимит 1000 узлов, нет rate limiting
4. Нет распределённого реестра (gossip) — каждый узел хранит только то, что услышал
5. Query/Response — 400ms sleep при cache miss
6. Суффикс домена кэшируется в NSS на время жизни процесса

## Roadmap
- Gossip-based sync реестра
- Шифрование payload (NaCl/Box)
- Rate limiting / DoS protection
- Web UI / визуализация топологии
- Service discovery (авторегистрация сервисов)
