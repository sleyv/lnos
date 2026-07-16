# LNOS — Summary

## Репозиторий
- **URL:** https://github.com/sleyv/lnos (форк Teskum-Researches/lnos)
- **Ветка:** master
- **Последний коммит:** `93349e9` — TLD blacklist + atomic owners.db
- **Uncommitted:** косметические правки в `main.cpp` (подсказки), `setup.sh` (multi-pm), `uninstall.sh`

## Язык и сборка
- **C++26** (GCC 16.1.1), fallback C++23 → C++20
- **Зависимости:** libsodium (Ed25519 подпись/верификация)
- **Сборка без интернета:** `cmake -DBUILD_TESTING=OFF` (GTest не подтягивается)
- **Артефакты:** `lnosd`, `lnosctl`, `libnss_lnos.so.2`

## Архитектура
- **Схема имён:** `device.type.owner` (например, `desktop.work.ruslan`)
- **Dual-stack:** независимые приёмники/передатчики для IPv4 и IPv6
- **Автоопределение интерфейса:** `getifaddrs()` — первый non-loopback UP/RUNNING
- **`IP_MULTICAST_IF` / `IPV6_MULTICAST_IF`** — привязка к обнаруженному интерфейсу (и для announce, и для query)
- **UNIX-сокет:** `~/.config/lnos/lnosd.sock` (не `/tmp` — C-5 fix)

## NSS-модуль (libnss_lnos.so.2)
- **TLD blacklist:** ~40 популярных TLD (`.com`, `.org`, `.ru`, `.io`, `.app`, `.dev`, `.local` и др.) — скипаются без mmap/сокета
- **Отключение blacklist:** `LNOS_SKIP_TLDS=0`
- **mmap owner check:** `/dev/shm/lnos_owners` — NSS проверяет owner (последняя часть имени) через mmap, без syscall'ов
- Если owner есть — идёт в UNIX-сокет демона
- Если owner нет — сразу `NOT_FOUND`
- Суффикс домена кэшируется один раз при загрузке модуля

## Конфигурация (всё без хардкода)
- **`lnosctl set`:** `domain`, `mcast_group`, `mcast_group_v6`, `port`, `name`
- **Дефолты:** `.gervaty`, `239.255.42.99:4545`, `ff02::4299:4545`
- **Config dir:** XDG (`~/.config/lnos`) → `/etc/lnos`
- **XDG_CONFIG_HOME** поддерживается

## owners.db
- Пишется **атомарно** (tmp + rename + permissions перед rename)
- Содержит список owner'ов, известных демону
- Обновляется при добавлении/удалении узлов
- Сидится своим owner'ом при старте

## Безопасность (все исправлено)
| CVE-like | Проблема | Статус |
|----------|----------|--------|
| C-1 | Приватные ключи world-readable | 750/644, ручной chmod 600 |
| C-2 | Name takeover (нет проверки подписи) | Проверка publicKey при привязке |
| C-3 | sodium_init() не вызывался | Вызов в main() |
| C-4 | OOM через nodes map (никогда не удалялись) | 1000 лимит, eviction через 4×TTL |
| C-5 | UNIX-сокет в /tmp (symlink race) | Перенесён в ~/.config/lnos |
| H-3/H-4 | OOM через decode (uint64_t длина строки/сервисов) | Лимит 1024 байта / 256 сервисов |
| H-5 | Single-threaded query server | Каждый клиент в отдельном треде |

## UB / Code quality
- `union PacketAs` → прямое хранение (UB)
- `*(uint64_t*)` → `std::memcpy` (strict aliasing + ARM)
- `std::shared_mutex` вместо `std::mutex`
- `default: return false` в decode
- Мёртвый код удалён

## Тесты
- **12/12 GTest** — все зелёные
- Покрытие: encode/decode (все типы пакетов, пустые имена, дубли портов, лимиты), Ed25519 подпись/верификация (включая tamper), config dir resolution (включая XDG)

## Развёртывание
- **setup.sh** — автоопределение пакетного менеджера (apt/pacman/dnf/zypper/emerge/apk), сборка, установка NSS, nsswitch.conf, systemd unit (или OpenRC), firewall, seeding owners.db
- **uninstall.sh** — полный откат
- **systemd:** `lnosd.service` с `Restart=on-failure`
- **Firewall:** ufw / firewalld / iptables — открытие multicast группы

## Известные ограничения при развёртывании на 10+ серверах
1. **Одна multicast группа:** все узлы должны быть в одном L2-сегменте (или VPN) — multicast не роутится между подсетями без PIM
2. **Нет шифрования данных** (только подпись пакетов, payload открытый)
3. **Защита от DDoS:** только лимит 1000 узлов — нет rate limiting на пакеты/сек
4. **Единая точка отказа:** нет распределённого реестра (gossip) — каждый узел хранит только то, что услышал
5. **Query/Response** — 400ms sleep при cache miss (блокирует ответ клиенту, хоть и в отдельном треде)
6. **NSS суффикс** кэшируется на всё время жизни процесса — смена домена требует перезапуска всех NSS-клиентов

## Что дальше (roadmap)
- Gossip-based sync реестра
- Шифрование payload (NaCl)
- Rate limiting / DoS protection
- Web UI / визуализация топологии
- Service discovery (авторегистрация сервисов)
