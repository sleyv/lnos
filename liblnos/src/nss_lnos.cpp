#include <nss.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <cstdlib>
#include <arpa/inet.h>
#include <lnos/config.h>

extern "C" {

static bool tldSkipped(const std::string& name) {
    static const char* skip[] = {
        ".com", ".org", ".net", ".edu", ".gov", ".mil",
        ".ru", ".рф", ".su", ".by", ".kz", ".ua",
        ".de", ".fr", ".uk", ".it", ".es", ".pl",
        ".cn", ".jp", ".kr", ".in", ".br",
        ".io", ".ai", ".app", ".dev", ".me", ".cc",
        ".info", ".biz", ".pro", ".name", ".mobi",
        ".site", ".online", ".store", ".blog", ".tech",
        ".xyz", ".top", ".club", ".win", ".bid",
        ".eu", ".asia", ".int", ".arpa",
        ".local", ".localhost", ".invalid", ".test",
        nullptr
    };
    static bool enabled = true;
    static bool checked = false;
    if (!checked) {
        const char* env = std::getenv("LNOS_SKIP_TLDS");
        if (env && std::string(env) == "0") enabled = false;
        checked = true;
    }
    if (!enabled) return false;

    for (const char** tld = skip; *tld; ++tld) {
        std::string s(*tld);
        if (name.size() > s.size() &&
            name.substr(name.size() - s.size()) == s) {
            return true;
        }
    }
    return false;
}

static bool ownerActive(const std::string& owner) {
    std::string path = lnos::getConfigDir() + "/owners.db";
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat sb;
    if (fstat(fd, &sb) < 0 || sb.st_size == 0) {
        close(fd);
        return false;
    }

    char* addr = static_cast<char*>(mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, fd, 0));
    close(fd);
    if (addr == MAP_FAILED) return false;

    std::string contents(addr, sb.st_size);
    munmap(addr, sb.st_size);

    size_t pos = 0;
    while (pos < contents.size()) {
        auto nl = contents.find('\n', pos);
        if (nl == std::string::npos) nl = contents.size();
        if (nl - pos == owner.size() && contents.compare(pos, owner.size(), owner) == 0) {
            return true;
        }
        pos = nl + 1;
    }
    return false;
}

static std::string extractOwner(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    return name.substr(dot + 1);
}

enum nss_status _nss_lnos_gethostbyname2_r(
    const char *name,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop)
{
    std::string dname(name);

    if (tldSkipped(dname)) {
        *errnop = ENOENT;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    std::string owner = extractOwner(dname);
    if (owner.empty() || !ownerActive(owner)) {
        *errnop = ENOENT;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    if (af != AF_INET && af != AF_INET6) {
        *errnop = EAFNOSUPPORT;
        *h_errnop = NO_DATA;
        return NSS_STATUS_UNAVAIL;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        *errnop = EAGAIN;
        *h_errnop = TRY_AGAIN;
        return NSS_STATUS_TRYAGAIN;
    }

    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un un{};
    un.sun_family = AF_UNIX;
    std::string socket_path = lnos::getConfigDir() + "/lnosd.sock";
    std::strncpy(un.sun_path, socket_path.c_str(), sizeof(un.sun_path) - 1);
    un.sun_path[sizeof(un.sun_path) - 1] = '\0';

    if (connect(sock, (struct sockaddr*)&un, sizeof(un)) < 0) {
        close(sock);
        *errnop = ECONNREFUSED;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    std::string query = dname + "\n";
    if (write(sock, query.data(), query.length()) < 0) {
        close(sock);
        *errnop = EAGAIN;
        return NSS_STATUS_TRYAGAIN;
    }

    char resp_buf[256];
    ssize_t n = read(sock, resp_buf, sizeof(resp_buf) - 1);
    close(sock);

    if (n <= 0) {
        *errnop = ENOENT;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    resp_buf[n] = '\0';
    std::string ip_str(resp_buf);
    while (!ip_str.empty() && (ip_str.back() == '\n' || ip_str.back() == '\r')) {
        ip_str.pop_back();
    }

    if (ip_str.empty() || ip_str == "NOT_FOUND") {
        *errnop = ENOENT;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    size_t name_len = dname.length() + 1;
    size_t addr_len = (af == AF_INET) ? 4 : 16;
    size_t required = name_len + addr_len + sizeof(char*) * 2 + sizeof(char*) + 16;

    if (buflen < required) {
        *errnop = ERANGE;
        return NSS_STATUS_TRYAGAIN;
    }

    char* addr_ptr = buffer;
    if (inet_pton(af, ip_str.c_str(), addr_ptr) != 1) {
        *errnop = ENOENT;
        *h_errnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    char* name_ptr = addr_ptr + addr_len;
    std::memcpy(name_ptr, dname.c_str(), name_len);

    char** addr_list = (char**)(name_ptr + name_len);
    size_t align_offset = (reinterpret_cast<uintptr_t>(addr_list) % alignof(char*));
    if (align_offset > 0) {
        addr_list = (char**)(reinterpret_cast<char*>(addr_list) + (alignof(char*) - align_offset));
    }

    addr_list[0] = addr_ptr;
    addr_list[1] = nullptr;

    result->h_name = name_ptr;
    result->h_aliases = addr_list + 1; 
    result->h_addrtype = af;
    result->h_length = addr_len;
    result->h_addr_list = addr_list;

    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_lnos_gethostbyname_r(
    const char *name,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop)
{
    return _nss_lnos_gethostbyname2_r(name, AF_INET, result, buffer, buflen, errnop, h_errnop);
}

} // extern "C"
