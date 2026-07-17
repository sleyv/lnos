# LNOS — Local Network Overlay System

**Форк** [Teskum-Researches/lnos](https://github.com/Teskum-Researches/lnos) с существенной переработкой архитектуры, новыми протоколами, шифрованием, HTTP-дашбордом, NSS-интеграцией, gossip-синхронизацией и универсальным установщиком.

## Что нового / Отличия от оригинала

### Dual-Stack IPv4/IPv6
Автоопределение активного интерфейса, независимые приёмники/передатчики. На IPv6 сокет установлен `IPV6_V6ONLY` — иначе он перехватывает IPv4 трафик.

### Шифрование
- **Multicast (symmetric):** `crypto_secretbox` с ключом = SHA256 от pubkey отправителя
- **Unicast (asymmetric):** `crypto_box` (X25519 + XSalsa20 + Poly1305)
- Все gossip-пакеты шифруются unicast на публичный ключ пира

### Gossip-протокол
Периодическая (30 с) синхронизация реестра между узлами: случайный online-пир получает весь список известных узлов, получатель merge'ит. Обеспечивает конвергенцию реестра без reliance только на multicast.

### Query/Response
Если демон не знает имя — рассылает multicast Query. Целевой узел отвечает unicast Response. После этого имя кэшируется.

### HTTP Dashboard
Встроенный HTTP-сервер (порт 9999, настраивается):
- `GET /` — HTML с auto-refresh, таблица узлов, метрики
- `GET /nodes` — JSON всех узлов
- `GET /stats` — JSON метрики демона

### NSS-модуль (libnss_lnos.so.2)
Системная интеграция: `getaddrinfo("device.type.owner")` работает в любых программах — ssh, ping, curl, getent. TLD blacklist, mmap `owners.db`, UNIX-сокет для получения IP.

### Daemon class (RAII)
Глобальное состояние → поля класса `Daemon`. Все сокеты через `FdGuard` (RAII, move-only). Trait-based IPv4/IPv6 — нет дублирования sender/receiver.

### Per-source rate limiting
50 пакетов/сек с одного источника (вместо глобального лимита), сброс раз в секунду.

### Self-registration
Узел регистрирует себя сам, не полагаясь на multicast loopback (не работает на некоторых WiFi). Резолвинг своего имени работает всегда.

### Graceful shutdown
Атомарный `running = false`, все потоки завершаются чисто.

### Протокол — Big Endian
Все multi-byte поля в сетевом порядке байт.

### Конфигурация
`lnosctl set`: name, mcast_group, mcast_group_v6, port, http_port, domain. Без хардкода.

### setup.sh — универсальный установщик
- Автоопределение пакетного менеджера (apt/pacman/dnf/zypper/emerge/apk)
- Три варианта имени (hostname/random/manual)
- Firewall: ufw → firewalld → nftables → iptables
- systemd / OpenRC
- `setup.sh --uninstall` — полное удаление

### Security
- Подписи Ed25519: name takeover невозможен — пакет с несовпадающим pubkey отклоняется
- Приватные ключи: 750 на директорию, 644 на файлы
- UNIX-сокет в `~/.config/lnos/` (не в `/tmp` — symlink race)
- Лимиты: 1024 байта строка, 256 сервисов, 1000 gossip-узлов, 1000 nodes
- Query server: каждый клиент в отдельном треде, backlog 128

### Owners.db atomic write
Через tmp + rename — crash-safe.

### Big Endian протокол
Бинарный протокол в сетевом порядке байт.

## Тесты
- **49/49 GTest** — encode/decode всех типов пакетов (включая GossipRequest/Response), Ed25519 (tamper), шифрование (multicast + unicast + decrypt чужим ключом), big-endian, config, readFile, gossip с IPv6, лимиты строк/сервисов/узлов, неизвестные версии/типы пакетов
- **Интеграционные:** `tests/integration.sh` — 3 Incus-контейнера, 29 тестов

## Известные ограничения
1. Multicast не роутится между L2-сегментами без PIM
2. Gossip — случайный пир, медленная конвергенция на 100+ узлах
3. HTTP UI без аутентификации
4. Нет persist-реестра между перезапусками
5. Нет автообновления сервисов без рестарта узла
