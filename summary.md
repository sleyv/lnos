# LNOS — Local Network Overlay System

## Репозиторий
- **URL:** https://github.com/sleyv/lnos (форк Teskum-Researches/lnos)
- **Ветка:** master

## Язык и сборка
- **C++26** (GCC 16.1.1), fallback C++23 → C++20. В CMake автоматический выбор стандарта.
- **Зависимости:** libsodium (Ed25519 подпись/верификация, шифрование)
- **Сборка без интернета:** `cmake -DBUILD_TESTING=OFF` (GTest не подтягивается)
- **Артефакты:** `lnosd`, `lnosctl`, `libnss_lnos.so.2`

## Все фичи (текущее состояние)

### Dual-Stack IPv4/IPv6
Демон через `getifaddrs()` определяет активный сетевой интерфейс (первый non-loopback UP/RUNNING) и запускает независимые приёмники/передатчики для IPv4 и IPv6. На IPv6 сокет установлен `IPV6_V6ONLY` — иначе он перехватывает IPv4 трафик и bind падает с `EADDRINUSE`.

### RAII-сокеты (FdGuard)
Класс `FdGuard` в main.cpp: закрытие сокета в деструкторе, move-семантика, запрет копирования. Все 6 функций (sender/receiver IPv4/IPv6, query server, HTTP server) используют его — ручной `close()` больше нигде не нужен.

### Trait-based IPv4/IPv6 deduplication
`IPv4Traits` и `IPv6Traits` — шаблонные параметры для `runSender<Traits>` и `runReceiver<Traits>`. Разница в типах сокетов, опциях, адресах вынесена в traits — нет дублирования кода для IPv4/IPv6.

### Daemon class (глобалы → поля)
Всё глобальное состояние (`nodes`, `nodesMutex`, `cfg`, `interfaceInfo`, `running`, `activeQueries`, счётчики) перенесено в поля `Daemon`. Методы (`runReceiver`, `runSender`, `runQueryServer`, `runHTTPServer`, `runGossip`) — методы класса.

###Self-registration узла
Узел регистрирует себя в `nodes` map при старте, не полагаясь на multicast loopback (который не работает на некоторых WiFi-драйверах). Резолвинг собственного имени работает всегда.

### NSS-модуль (libnss_lnos.so.2)
Системная интеграция: `getaddrinfo("device.type.owner")` работает в любых программах (ssh, ping, curl, getent). Модуль:
1. Проверяет TLD blacklist — если имя заканчивается на известный TLD (`.com`, `.org`, `.ru` и т.д.) → сразу `NOT_FOUND`
2. Извлекает owner (последняя часть имени) и проверяет через mmap `owners.db`
3. Если owner в сети — идёт в UNIX-сокет демона и получает IP
4. Если нет — `NOT_FOUND` без единого syscall'а

Отключение TLD blacklist: `LNOS_SKIP_TLDS=0`.

### Query/Response протокол
Два новых типа пакетов. Если демон не знает имя в локальном реестре — рассылает multicast Query. Целевой узел отвечает unicast Response с IP. После этого имя кэшируется в реестре.

### Шифрование payload
Два режима:
- **Multicast (symmetric):** `crypto_secretbox` с ключом = SHA256 от публичного ключа отправителя. Все участники сети могут расшифровать.
- **Unicast (asymmetric):** `crypto_box` (X25519 + XSalsa20 + Poly1305) с конвертацией Ed25519→Curve25519. Для gossip-ответов.

Поле `isEncrypted` (0/1/2) в протоколе. Nonce (24 байта) случайный, передаётся в пакете. Тест `PayloadEncryption` покрывает оба режима.

### Gossip-протокол
- Новые типы пакетов: `GossipRequest` (3), `GossipResponse` (4)
- Каждые 30 секунд узел выбирает случайного Online-пира и шлёт ему свой реестр
- Получатель merge'ит полученные узлы в свой реестр
- Gossip-пакеты шифруются unicast (crypto_box на публичный ключ пира)
- Тест `GossipSerialization` покрывает encode/decode

### HTTP Server UI
Встроенный HTTP-сервер на порту 9999:
- `GET /` — HTML-дашборд с auto-refresh (2 сек), метрики, таблица узлов
- `GET /nodes` — JSON-список всех узлов с их сервисами и статусом
- `GET /stats` — JSON-метрики демона

### Per-source rate limiting
- Глобальный лимит заменён на per-IP: до 50 пакетов/сек с одного источника
- Сброс счётчиков раз в секунду
- Превышение → дропаются пакеты только с этого IP

### Metrics (атомарные счётчики)
- `queriesResolved`, `queriesFailed`, `packetsReceived`, `packetsDropped`, `packetsRejectedSig`
- Все `std::atomic<uint64_t>`, доступны через HTTP API и `lnosctl stats`

### Предпочтение IPv4
В ресивере: если у узла уже есть IPv4-адрес, а новый announce пришёл по IPv6 — IP не перезаписывается. Предотвращает резолв в link-local IPv6 (`fe80::...`).

### Привязка multicast к интерфейсу
`IP_MULTICAST_IF` (по IPv4-адресу) и `IPV6_MULTICAST_IF` (по ifindex) при отправке Announce и Query. Пакеты уходят через обнаруженный интерфейс, а не через дефолтный маршрут.

### Конфигурация (без хардкода)
`lnosctl set`: `domain`, `mcast_group`, `mcast_group_v6`, `port`, `name`. Дефолты: `.gervaty`, `239.255.42.99:4545`, `ff02::4299:4545`.

### Atomic owners.db
`owners.db` пишется через tmp + rename — crash-safe. Права выставляются до rename.

### Graceful shutdown
Атомарный `running = false`, все потоки завершаются чисто.

### Big Endian протокол
Бинарный протокол использует сетевой порядок байт (Big Endian).

### Firewall rule fix
UFW правило `from 224.0.0.0/4` → `Anywhere`. Старая версия блокировала multicast loopback — узел не слышал свои announce и уходил в Offline.

## Security-фиксы

| ID | Проблема | Решение |
|----|----------|---------|
| C-1 | Приватные ключи world-readable (umask) | `createConfig()`: 750 на директорию, 644 на файлы |
| C-2 | Name takeover — подписи не проверялись при привязке имени | В `Node` добавлено `publicKey`. Несовпадение ключа → пакет отклонён |
| C-3 | `sodium_init()` не вызывался — краш или тихая некорректная подпись | `sodium_init()` с проверкой в `main()`. Ключи грузятся однократно через `static` |
| C-4 | OOM через nodes map — clean-up только ставил `Offline`, не удаляя | Мёртвые ноды удаляются через 4×TTL. Лимит — 1000 узлов |
| C-5 | UNIX-сокет в `/tmp` — symlink race (LPE) | Сокет в `~/.config/lnos/lnosd.sock` |
| H-3/H-4 | OOM через decode — `uint64_t` длина строки/сервисов | Лимит строки 1024 байта, сервисов 256, gossip-узлов 1000 |
| H-5 | Single-threaded query server — блокировка accept | Каждый клиент в отдельном треде. Backlog 128 |

## UB Fixes & Code Quality

- `union PacketAs` → прямое хранение (UB с `std::string` + `std::vector`)
- `*(uint64_t*)packet.data` → `std::memcpy` (strict aliasing + bus error на ARM)
- `default: return false` в decode — неизвестный тип пакета не читает мусор
- `std::shared_mutex` вместо `std::mutex` — читатели не блокируют друг друга
- RAII сокеты — ручной `close()` заменён на `FdGuard`
- Trait-based IPv4/IPv6 — дублирование кода sender/receiver устранено

## Тесты
- **29/29 GTest** — все зелёные
- Покрытие: encode/decode всех типов пакетов (включая GossipRequest/Response), пустые имена, дубли портов, лимиты строк и сервисов, Ed25519 подпись/верификация (tamper), шифрование payload (multicast + unicast), big-endian, blob push/consume, config dir resolution (XDG), config set/load, readFile

## Развёртывание
- **setup.sh** — автоопределение пакетного менеджера (apt/pacman/dnf/zypper/emerge/apk), сборка, установка бинарников в `/usr/local/bin/`, NSS + nsswitch.conf + systemd/OpenRC + firewall + seed owners.db
- **uninstall.sh** — полный откат: остановка демона, удаление бинарников, NSS, откат nsswitch.conf, очистка firewall, удаление конфигов и build
- **systemd:** `lnosd.service` с `Restart=on-failure`, `After=network-online.target`

## Известные ограничения
1. Multicast не роутится между L2-сегментами без PIM — все узлы в одной подсети/VPN
2. Gossip выбирает случайного пира — может не сходиться быстро на больших сетях (100+ узлов)
3. HTTP UI без аутентификации — не для публичных сетей
4. Нет persist-реестра после перезапуска демона
5. Нет автообновления сервисов (порт/имя меняются только через рестарт узла)
6. Суффикс домена кэшируется в NSS на время жизни процесса

## Roadmap (что ещё можно)
- Persistent registry (dump/restore реестра на диск)
- Service discovery (авторегистрация и поиск по типу сервиса)
- Direct messaging между узлами
- Prometheus metrics endpoint
- mDNS compatibility (RFC 6762)
- NAT traversal / relay nodes
