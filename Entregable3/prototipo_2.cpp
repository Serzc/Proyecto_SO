#include "dsm.h"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <ifaddrs.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <csignal>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

// g++ -std=c++17 -pthread -Wall -Wextra -Wpedantic "Entregable 3/prototipo_2.cpp" -o dsmnode

namespace dsm {

constexpr std::size_t MAX_NODES = 32;
constexpr std::size_t MAX_PENDING = 256;
constexpr std::size_t QUERY_MAX = 256;
constexpr std::uint8_t NODE_NONE = 0xFF;
constexpr std::uint32_t PAGE_NONE = 0xFFFFFFFF;
constexpr std::uint8_t HEARTBEAT_PORT_ROLE = 2;
constexpr std::uint8_t HEARTBEAT_MESSAGE = 3;
constexpr std::uint8_t DEMO_MESSAGE = 0x80;
constexpr std::uint8_t DEMO_BROADCAST = 1;
constexpr std::uint8_t DEMO_UNICAST = 2;
constexpr std::size_t MESSAGE_HEADER_SIZE = 16;
constexpr std::size_t DEMO_MAX_BYTES = 1024;
constexpr std::size_t ALLOC_REQUEST_SIZE = 8;
constexpr std::size_t FREE_REQUEST_SIZE = 12;
constexpr std::size_t PAGE_INDEX_SIZE = 4;
constexpr std::uint8_t STATUS_OK = 0;
constexpr std::uint8_t STATUS_NO_SPACE = 1;
constexpr std::uint8_t STATUS_INVALID = 2;
constexpr std::size_t CHANNEL_COUNT = 3;
std::mutex console_output_mutex;

// Demo-only message type; this does not add a DSM operation to section 5.
// Each broadcast is sent as one unicast TCP frame to every configured peer.
// MSI access policy below models the section 2.4 transitions; data messages
// do not perform page transfers or change page-table states.
enum class Channel : std::uint8_t {
    Data = 0,
    Control = 1,
    Heartbeat = HEARTBEAT_PORT_ROLE
};

enum class NodeState : std::uint8_t {
    Offline = 0,
    Online = 1
};

enum class PageState : std::uint8_t {
    Invalid = 0,
    Shared = 1,
    Modified = 2
};

enum class PageAccess {
    Read,
    Write
};

struct MsiAccessPolicy {
    PageState granted_state;
    bool request_home;
    bool invalidate_other_copies;
    bool retry_access;
};

MsiAccessPolicy msiPolicyForAccess(PageState state, PageAccess access) {
    if (state == PageState::Modified ||
        (state == PageState::Shared && access == PageAccess::Read)) {
        return {state, false, false, false};
    }
    if (state == PageState::Invalid) {
        return {PageState::Shared, true, false, access == PageAccess::Write};
    }
    return {PageState::Modified, true, true, false};
}

PageState msiOwnerStateAfterRequest(PageState owner_state, PageAccess access) {
    if (access == PageAccess::Read && owner_state == PageState::Modified) {
        return PageState::Shared;
    }
    if (access == PageAccess::Write) {
        return PageState::Invalid;
    }
    return owner_state;
}

constexpr PageState msiStateAfterInvalidation() {
    return PageState::Invalid;
}

enum class MessageType : std::uint8_t {
    Join = 1,
    JoinAck = 2,
    Heartbeat = HEARTBEAT_MESSAGE,
    PageRequest = 4,
    PageData = 5,
    Invalidate = 6,
    InvalidateAck = 7,
    AllocRequest = 8,
    AllocReply = 9,
    Free = 10,
    QueryRequest = 11,
    QueryReply = 12,
    FreeReply = 13
};

struct MsgHeader {
    std::uint8_t type;
    std::uint8_t sender;
    std::uint8_t status;
    std::uint8_t flags;
    std::uint32_t seq;
    std::uint32_t page;
    std::uint32_t payload_len;
};
static_assert(sizeof(MsgHeader) == 16, "MsgHeader debe ocupar 16 bytes");

struct NodeInfo {
    std::uint32_t ipv4;       // Dirección IPv4 en orden de red.
    std::uint32_t first_page; // node_id * pages_per_node.
    std::uint32_t num_pages;
    std::uint8_t node_id;
    std::uint8_t state;
    std::uint16_t reserved;
};
static_assert(sizeof(NodeInfo) == 16, "NodeInfo debe ocupar 16 bytes");

struct PageEntry {
    std::uint32_t proc_id;
    std::uint32_t copyset;
    std::uint32_t order;
    std::uint8_t state;
    std::uint8_t owner;
    std::uint16_t reserved;
};
static_assert(sizeof(PageEntry) == 16, "PageEntry debe ocupar 16 bytes");

struct NodeStats {
    std::uint32_t total_pages;
    std::uint32_t free_pages;
    std::uint32_t pages_m;
    std::uint32_t pages_s;
    std::uint32_t pages_i;
    std::uint32_t faults;
    std::uint32_t pages_sent;
    std::uint32_t pages_recv;
};
static_assert(sizeof(NodeStats) == 32, "NodeStats debe ocupar 32 bytes");

struct AllocReq {
    std::uint32_t npages;
    std::uint32_t proc_id;
};
static_assert(sizeof(AllocReq) == 8, "AllocReq debe ocupar 8 bytes");

struct QueryReq {
    std::uint32_t kind;
    std::uint32_t first_page;
    std::uint32_t count;
};
static_assert(sizeof(QueryReq) == 12, "QueryReq debe ocupar 12 bytes");

struct Pending {
    std::condition_variable ready;
    std::uint32_t seq;
    std::uint8_t dest;
    std::uint8_t response_type;
    std::uint8_t status = STATUS_OK;
    bool complete = false;
    int result = DSM_E_NET;
    std::vector<std::uint8_t> payload;
};

struct Allocation {
    std::uint32_t first_page;
    std::uint32_t num_pages;
    std::uint32_t proc_id;
};

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

bool readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    value = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
            static_cast<std::uint32_t>(bytes[offset + 3]);
    return true;
}

std::array<std::uint8_t, MESSAGE_HEADER_SIZE> encodeHeader(const MsgHeader& header) {
    std::array<std::uint8_t, MESSAGE_HEADER_SIZE> bytes{};
    bytes[0] = header.type;
    bytes[1] = header.sender;
    bytes[2] = header.status;
    bytes[3] = header.flags;
    const auto writeU32 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
        bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
    };
    writeU32(4, header.seq);
    writeU32(8, header.page);
    writeU32(12, header.payload_len);
    return bytes;
}

bool decodeHeader(const std::array<std::uint8_t, MESSAGE_HEADER_SIZE>& bytes,
                  MsgHeader& header) {
    header.type = bytes[0];
    header.sender = bytes[1];
    header.status = bytes[2];
    header.flags = bytes[3];
    const auto readField = [&bytes](std::size_t offset, std::uint32_t& value) {
        value = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
                static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    readField(4, header.seq);
    readField(8, header.page);
    readField(12, header.payload_len);
    return true;
}

struct Config {
    std::uint64_t dsm_base = 0x100000000000ULL;
    std::uint32_t page_size = 4096;
    std::uint32_t pages_per_node = 256;
    std::uint32_t heartbeat_ms = 1000;
    std::uint32_t timeout_ms = 5000;
    std::uint16_t port_base = 28400;
    std::uint16_t num_hosts = 0;
    std::uint8_t reserved[4]{};
    char hosts[MAX_NODES][64]{};
};
static_assert(sizeof(Config) == 2080, "Config debe ocupar 2080 bytes");

class NodeTable {
public:
    using Clock = std::chrono::steady_clock;

    NodeTable(std::size_t node_count, std::uint8_t local_node_id,
              std::uint32_t pages_per_node)
        : node_count_(node_count), local_node_id_(local_node_id) {
        if (node_count == 0 || node_count > MAX_NODES ||
            local_node_id >= node_count ||
            pages_per_node > UINT32_MAX / node_count) {
            throw std::invalid_argument("Configuración inválida para la tabla de nodos");
        }

        for (std::size_t id = 0; id < node_count_; ++id) {
            entries_[id].node_id = static_cast<std::uint8_t>(id);
            entries_[id].first_page = static_cast<std::uint32_t>(id) * pages_per_node;
            entries_[id].num_pages = pages_per_node;
            entries_[id].state = static_cast<std::uint8_t>(NodeState::Offline);
            stats_[id].total_pages = pages_per_node;
            stats_[id].free_pages = pages_per_node;
            stats_[id].pages_i = pages_per_node;
        }
    }

    void configureNode(std::uint8_t node_id, std::uint32_t ipv4) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_) {
            throw std::out_of_range("node_id fuera de la tabla");
        }
        entries_[node_id].ipv4 = ipv4;
    }

    void markLocalOnline() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[local_node_id_].state = static_cast<std::uint8_t>(NodeState::Online);
        last_seen_[local_node_id_] = Clock::now();
    }

    bool recordContact(std::uint8_t node_id, std::uint32_t source_ipv4) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_ || node_id == local_node_id_ ||
            entries_[node_id].ipv4 != source_ipv4) {
            return false;
        }

        entries_[node_id].state = static_cast<std::uint8_t>(NodeState::Online);
        last_seen_[node_id] = Clock::now();
        return true;
    }

    void markTimedOut(std::chrono::milliseconds timeout) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t id = 0; id < node_count_; ++id) {
            if (id != local_node_id_ &&
                entries_[id].state == static_cast<std::uint8_t>(NodeState::Online) &&
                now - last_seen_[id] > timeout) {
                entries_[id].state = static_cast<std::uint8_t>(NodeState::Offline);
            }
        }
    }

    std::vector<NodeInfo> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<NodeInfo>(entries_.begin(), entries_.begin() + node_count_);
    }

    std::vector<NodeStats> statsSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<NodeStats>(stats_.begin(), stats_.begin() + node_count_);
    }

    void recordAllocation(std::uint8_t node_id, std::uint32_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_ || count > stats_[node_id].free_pages) {
            throw std::logic_error("Contadores de asignación fuera de rango");
        }
        stats_[node_id].free_pages -= count;
    }

    void recordFree(std::uint8_t node_id, std::uint32_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_ ||
            count > stats_[node_id].total_pages - stats_[node_id].free_pages) {
            throw std::logic_error("Contadores de liberación fuera de rango");
        }
        stats_[node_id].free_pages += count;
    }

    // Cuenta un fallo atendido y mueve la página entre los contadores M/S/I.
    void recordPageState(std::uint8_t node_id, PageState from, PageState to,
                         bool fault) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_) {
            throw std::logic_error("Nodo fuera de rango en estadísticas MSI");
        }
        NodeStats& stats = stats_[node_id];
        const auto counter = [&stats](PageState state) -> std::uint32_t& {
            return state == PageState::Modified ? stats.pages_m
                 : state == PageState::Shared   ? stats.pages_s
                                                : stats.pages_i;
        };
        if (from != to) {
            --counter(from);
            ++counter(to);
        }
        if (fault) {
            ++stats.faults;
        }
    }

private:
    const std::size_t node_count_;
    const std::uint8_t local_node_id_;
    mutable std::mutex mutex_; // Protege entradas, estadísticas y última actividad.
    std::array<NodeInfo, MAX_NODES> entries_{};
    std::array<NodeStats, MAX_NODES> stats_{};
    std::array<Clock::time_point, MAX_NODES> last_seen_{};
};

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::uint64_t parseUnsigned(const std::string& value, const std::string& key) {
    std::size_t parsed = 0;
    std::uint64_t result = 0;
    try {
        result = std::stoull(value, &parsed, 0);
    } catch (const std::exception&) {
        throw std::runtime_error("Valor inválido para " + key + ": " + value);
    }
    if (parsed != value.size()) {
        throw std::runtime_error("Valor inválido para " + key + ": " + value);
    }
    return result;
}

Config loadConfig(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("No se pudo abrir el archivo de configuración: " + path);
    }

    Config config;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("Falta '=' en " + path + ":" +
                                     std::to_string(line_number));
        }
        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("Parámetro vacío en " + path + ":" +
                                     std::to_string(line_number));
        }

        if (key == "host") {
            if (config.num_hosts == MAX_NODES) {
                throw std::runtime_error("La configuración excede MAX_NODES");
            }
            if (value.size() >= sizeof(config.hosts[0])) {
                throw std::runtime_error("El nombre/IP del host excede 63 caracteres");
            }
            std::memcpy(config.hosts[config.num_hosts], value.c_str(), value.size() + 1);
            ++config.num_hosts;
            continue;
        }

        const std::uint64_t number = parseUnsigned(value, key);
        if (key == "dsm_base") {
            config.dsm_base = number;
        } else if (key == "page_size" && number <= UINT32_MAX) {
            config.page_size = static_cast<std::uint32_t>(number);
        } else if (key == "pages_per_node" && number <= UINT32_MAX) {
            config.pages_per_node = static_cast<std::uint32_t>(number);
        } else if (key == "heartbeat_ms" && number <= UINT32_MAX) {
            config.heartbeat_ms = static_cast<std::uint32_t>(number);
        } else if (key == "timeout_ms" && number <= UINT32_MAX) {
            config.timeout_ms = static_cast<std::uint32_t>(number);
        } else if (key == "port_base" && number <= UINT16_MAX) {
            config.port_base = static_cast<std::uint16_t>(number);
        } else {
            throw std::runtime_error("Parámetro desconocido o fuera de rango: " + key);
        }
    }
    if (input.bad()) {
        throw std::runtime_error("Error leyendo el archivo de configuración: " + path);
    }

    if (config.num_hosts == 0) {
        throw std::runtime_error("La configuración necesita al menos una línea host");
    }
    if (config.page_size == 0 || config.page_size % 4096 != 0) {
        throw std::runtime_error("page_size debe ser múltiplo de 4096");
    }
    if (config.dsm_base == 0 || config.dsm_base % config.page_size != 0) {
        throw std::runtime_error("dsm_base debe ser distinto de cero y estar alineado a page_size");
    }
    if (config.pages_per_node == 0 || config.heartbeat_ms == 0 ||
        config.timeout_ms == 0) {
        throw std::runtime_error("pages_per_node, heartbeat_ms y timeout_ms deben ser positivos");
    }
    if (config.pages_per_node > UINT32_MAX / config.num_hosts) {
        throw std::runtime_error("El total de páginas no cabe en el espacio de páginas de 32 bits");
    }
    const std::uint32_t last_port =
        static_cast<std::uint32_t>(config.port_base) +
        3U * (static_cast<std::uint32_t>(config.num_hosts) - 1U) +
        HEARTBEAT_PORT_ROLE;
    if (last_port > UINT16_MAX) {
        throw std::runtime_error("El rango de puertos configurado excede 65535");
    }
    return config;
}

std::uint32_t resolveIPv4(const char* host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int error = getaddrinfo(host, nullptr, &hints, &result);
    if (error != 0) {
        throw std::runtime_error("No se pudo resolver el host '" + std::string(host) +
                                 "': " + gai_strerror(error));
    }

    const auto* address = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    const std::uint32_t ipv4 = address->sin_addr.s_addr;
    freeaddrinfo(result);
    return ipv4;
}

bool isLocalIPv4(std::uint32_t ipv4) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) < 0) {
        throw std::runtime_error("getifaddrs() falló al validar la IP local: " +
                                 std::string(std::strerror(errno)));
    }

    bool found = false;
    for (const ifaddrs* interface = interfaces; interface != nullptr;
         interface = interface->ifa_next) {
        if (interface->ifa_addr == nullptr ||
            interface->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* address =
            reinterpret_cast<const sockaddr_in*>(interface->ifa_addr);
        if (address->sin_addr.s_addr == ipv4) {
            found = true;
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

std::uint16_t heartbeatPort(const Config& config, std::uint8_t node_id) {
    return static_cast<std::uint16_t>(
        config.port_base + 3U * static_cast<std::uint16_t>(node_id) +
        HEARTBEAT_PORT_ROLE);
}

std::uint16_t channelPort(const Config& config, std::uint8_t node_id,
                          Channel channel) {
    return static_cast<std::uint16_t>(
        config.port_base + 3U * static_cast<std::uint16_t>(node_id) +
        static_cast<std::uint8_t>(channel));
}

// Estado global que lee el manejador de SIGSEGV. Solo hay una región DSM por
// proceso, porque un proceso solo puede tener un manejador de SIGSEGV.
namespace pager {

struct FaultMsg {
    std::uint32_t page;
    std::uint32_t write;
};

std::atomic<std::uintptr_t> region_base{0};
std::atomic<std::size_t> region_bytes{0};
std::atomic<std::size_t> page_bytes{0};
std::atomic<int> fault_fd{-1};
// Por página: el worker incrementa la generación al terminar cada solicitud.
std::atomic<std::uint32_t>* generations = nullptr;
std::atomic<std::uint8_t>* failed = nullptr;
struct sigaction previous_action {};

void restorePreviousHandler() {
    sigaction(SIGSEGV, &previous_action, nullptr);
}

// Solo usa operaciones async-signal-safe: atómicos lock-free, write y nanosleep.
void onSegv(int, siginfo_t* info, void* context) {
    const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    const std::uintptr_t base = region_base.load();
    if (base == 0 || address < base || address - base >= region_bytes.load()) {
        // No es memoria DSM: al reintentar la instrucción actúa el manejador previo.
        restorePreviousHandler();
        return;
    }

    const auto page = static_cast<std::uint32_t>((address - base) / page_bytes.load());
    // shortcut: bit 1 de REG_ERR indica escritura solo en Linux x86_64.
    const auto* ucontext = static_cast<const ucontext_t*>(context);
    const bool write_access = (ucontext->uc_mcontext.gregs[REG_ERR] & 2) != 0;

    const std::uint32_t generation = generations[page].load();
    const FaultMsg message{page, write_access ? 1U : 0U};
    if (::write(fault_fd.load(), &message, sizeof(message)) !=
        static_cast<ssize_t>(sizeof(message))) {
        restorePreviousHandler();
        return;
    }
    const timespec pause{0, 100000};
    while (generations[page].load() == generation) {
        nanosleep(&pause, nullptr);
    }
    if (failed[page].load() != 0) {
        static const char text[] = "dsm: fallo de página que este nodo no puede resolver\n";
        if (::write(STDERR_FILENO, text, sizeof(text) - 1) < 0) {
            // Nada más que hacer dentro del manejador.
        }
        restorePreviousHandler();
    }
    // Al retornar, el CPU reintenta el acceso con la nueva protección.
}

}  // namespace pager

int protectionFor(PageState state) {
    switch (state) {
    case PageState::Modified: return PROT_READ | PROT_WRITE;
    case PageState::Shared: return PROT_READ;
    case PageState::Invalid: break;
    }
    return PROT_NONE;
}

class Node {
public:
    Node(Config config, std::uint8_t node_id)
        : config_(config), node_id_(node_id),
          node_table_(config.num_hosts, node_id, config.pages_per_node) {
        if (node_id_ >= config_.num_hosts) {
            throw std::invalid_argument("node_id no pertenece a la configuración");
        }

        for (std::size_t id = 0; id < config_.num_hosts; ++id) {
            peer_addresses_[id].sin_family = AF_INET;
            peer_addresses_[id].sin_addr.s_addr = resolveIPv4(config_.hosts[id]);
            const auto peer_id = static_cast<std::uint8_t>(id);
            peer_addresses_[id].sin_port =
                htons(channelPort(config_, peer_id, Channel::Heartbeat));
            node_table_.configureNode(peer_id, peer_addresses_[id].sin_addr.s_addr);
        }

        if (!isLocalIPv4(peer_addresses_[node_id_].sin_addr.s_addr)) {
            char address_text[INET_ADDRSTRLEN]{};
            const char* configured_ip = inet_ntop(
                AF_INET, &peer_addresses_[node_id_].sin_addr,
                address_text, sizeof(address_text));
            throw std::invalid_argument(
                "node_id " + std::to_string(static_cast<unsigned int>(node_id_)) +
                " configura host '" + config_.hosts[node_id_] + "' (" +
                (configured_ip == nullptr ? std::string("IP no disponible")
                                          : std::string(configured_ip)) +
                "), pero esa IPv4 no está asignada a esta máquina");
        }

        const std::uint32_t total_pages =
            static_cast<std::uint32_t>(config_.num_hosts) * config_.pages_per_node;
        page_table_.resize(total_pages);
        for (auto& page : page_table_) {
            page.state = static_cast<std::uint8_t>(PageState::Invalid);
            page.owner = NODE_NONE;
        }
    }

    ~Node() {
        stop();
    }

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }

        try {
            setupMemory();
            fault_thread_ = std::thread(&Node::faultLoop, this);
            for (std::size_t role = 0; role < CHANNEL_COUNT; ++role) {
                listeners_[role] = openListener(static_cast<Channel>(role));
            }
            node_table_.markLocalOnline();
            joined_[node_id_] = true;
            network_thread_ = std::thread(&Node::networkLoop, this);
            heartbeat_thread_ = std::thread(&Node::heartbeatLoop, this);
            join_thread_ = std::thread(&Node::joinLoop, this);
        } catch (...) {
            running_ = false;
            failPendingRequests();
            if (network_thread_.joinable()) {
                network_thread_.join();
            }
            if (heartbeat_thread_.joinable()) {
                heartbeat_thread_.join();
            }
            if (join_thread_.joinable()) {
                join_thread_.join();
            }
            for (int& listener : listeners_) {
                if (listener >= 0) {
                    close(listener);
                    listener = -1;
                }
            }
            teardownMemory();
            throw;
        }
    }

    void stop() {
        running_ = false;
        failPendingRequests();
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        if (join_thread_.joinable()) {
            join_thread_.join();
        }
        for (int& listener : listeners_) {
            if (listener >= 0) {
                close(listener);
                listener = -1;
            }
        }

        joinThreads(user_threads_, user_threads_mutex_);
        // Las aplicaciones ya terminaron: se puede liberar la región DSM.
        teardownMemory();
    }

    void launchUserThread(std::function<void()> application) {
        if (!running_) {
            throw std::logic_error("El nodo debe iniciarse antes de lanzar una aplicación");
        }
        std::lock_guard<std::mutex> lock(user_threads_mutex_);
        user_threads_.emplace_back(std::move(application));
    }

    std::vector<NodeInfo> nodes() const {
        return node_table_.snapshot();
    }

    std::vector<NodeStats> stats() const {
        return node_table_.statsSnapshot();
    }

    bool copyPageTable(std::uint8_t target_node, std::uint32_t first_page,
                       std::uint32_t count, PageEntry* out) const {
        const auto node_entries = nodes();
        if (target_node >= node_entries.size()) {
            return false;
        }
        const NodeInfo& target = node_entries[target_node];
        if (first_page < target.first_page) {
            return false;
        }
        const std::uint32_t offset = first_page - target.first_page;
        if (offset > target.num_pages || count > target.num_pages - offset) {
            return false;
        }

        std::lock_guard<std::mutex> lock(page_table_mutex_);
        std::copy_n(page_table_.begin() + first_page, count, out);
        return true;
    }

    const Config& config() const {
        return config_;
    }

    std::uint8_t nodeId() const {
        return node_id_;
    }

    int allocate(std::size_t size, std::uint32_t proc_id, void** out) {
        if (size == 0 || proc_id == 0 || out == nullptr) {
            return DSM_E_INVAL;
        }
        const std::size_t page_size = config_.page_size;
        const std::size_t page_count =
            size / page_size + (size % page_size != 0 ? 1 : 0);
        if (page_count == 0 || page_count > config_.pages_per_node ||
            page_count > UINT32_MAX) {
            return DSM_E_INVAL;
        }

        const auto node_entries = nodes();
        std::vector<std::uint8_t> candidates;
        candidates.reserve(node_entries.size());
        const std::size_t first =
            allocation_cursor_.fetch_add(1) % node_entries.size();
        for (std::size_t offset = 0; offset < node_entries.size(); ++offset) {
            const auto candidate = static_cast<std::uint8_t>(
                (first + offset) % node_entries.size());
            if (candidate == node_id_ ||
                node_entries[candidate].state ==
                    static_cast<std::uint8_t>(NodeState::Online)) {
                candidates.push_back(candidate);
            }
        }

        int last_error = DSM_E_NOMEM;
        for (const std::uint8_t home : candidates) {
            std::uint32_t first_page = 0;
            int result = DSM_OK;
            if (home == node_id_) {
                result = allocateAtHome(static_cast<std::uint32_t>(page_count),
                                        proc_id, first_page);
            } else {
                std::vector<std::uint8_t> request;
                request.reserve(ALLOC_REQUEST_SIZE);
                appendU32(request, static_cast<std::uint32_t>(page_count));
                appendU32(request, proc_id);
                std::vector<std::uint8_t> response;
                result = requestMessage(home, MessageType::AllocRequest, PAGE_NONE,
                                        request, MessageType::AllocReply,
                                        response);
                if (result == DSM_OK &&
                    !readU32(response, 0, first_page)) {
                    result = DSM_E_NET;
                }
            }

            if (result == DSM_E_NOMEM) {
                continue;
            }
            if (result != DSM_OK) {
                last_error = result;
                continue;
            }

            const auto rollback = [&] {
                if (home == node_id_) {
                    return releaseAtHome(first_page,
                                         static_cast<std::uint32_t>(page_count),
                                         proc_id);
                }
                return sendFreeRequest(home, first_page,
                                       static_cast<std::uint32_t>(page_count),
                                       proc_id);
            };
            const std::uint64_t offset =
                static_cast<std::uint64_t>(first_page) * config_.page_size;
            if (offset > UINT64_MAX - config_.dsm_base) {
                rollback();
                return DSM_E_INVAL;
            }
            const std::uint64_t address = config_.dsm_base + offset;
            const std::uint64_t allocation_bytes =
                static_cast<std::uint64_t>(page_count) * config_.page_size;
            const std::uint64_t address_limit =
                std::numeric_limits<std::uintptr_t>::max();
            if (address > address_limit ||
                allocation_bytes == 0 ||
                allocation_bytes - 1 > address_limit - address) {
                rollback();
                return DSM_E_INVAL;
            }
            void* allocation_ptr =
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
            if (allocation_ptr == nullptr) {
                rollback();
                return DSM_E_INVAL;
            }
            try {
                std::lock_guard<std::mutex> lock(allocations_mutex_);
                const auto inserted = allocations_.emplace(
                    reinterpret_cast<std::uintptr_t>(allocation_ptr),
                    Allocation{first_page, static_cast<std::uint32_t>(page_count),
                               proc_id});
                if (!inserted.second) {
                    if (home == node_id_) {
                        releaseAtHome(first_page,
                                      static_cast<std::uint32_t>(page_count),
                                      proc_id);
                    } else {
                        sendFreeRequest(home, first_page,
                                        static_cast<std::uint32_t>(page_count),
                                        proc_id);
                    }
                    return DSM_E_BADADDR;
                }
            } catch (const std::bad_alloc&) {
                if (home == node_id_) {
                    releaseAtHome(first_page, static_cast<std::uint32_t>(page_count),
                                  proc_id);
                } else {
                    sendFreeRequest(home, first_page,
                                    static_cast<std::uint32_t>(page_count), proc_id);
                }
                throw;
            }
            *out = allocation_ptr;
            return DSM_OK;
        }
        return last_error;
    }

    int release(void* ptr) {
        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        Allocation allocation{};
        {
            std::lock_guard<std::mutex> lock(allocations_mutex_);
            const auto found = allocations_.find(address);
            if (found == allocations_.end()) {
                return DSM_E_BADADDR;
            }
            allocation = found->second;
        }

        const auto home = static_cast<std::uint8_t>(
            allocation.first_page / config_.pages_per_node);
        const int result = home == node_id_
            ? releaseAtHome(allocation.first_page, allocation.num_pages,
                            allocation.proc_id)
            : sendFreeRequest(home, allocation.first_page,
                              allocation.num_pages, allocation.proc_id);
        if (result == DSM_OK) {
            std::lock_guard<std::mutex> lock(allocations_mutex_);
            allocations_.erase(address);
        }
        return result;
    }

    bool sendDemoMessage(std::optional<std::uint8_t> target, Channel channel,
                         const std::string& message) const {
        if (channel == Channel::Heartbeat || message.empty() ||
            message.size() > DEMO_MAX_BYTES || message.size() > config_.page_size) {
            return false;
        }

        const auto sendTo = [&](std::uint8_t peer_id, std::uint8_t flags) {
            MsgHeader header{};
            header.type = DEMO_MESSAGE;
            header.sender = node_id_;
            header.flags = flags;
            header.seq = next_sequence_.fetch_add(1);
            header.page = PAGE_NONE;
            return sendFrame(peer_id, channel, header,
                             reinterpret_cast<const std::uint8_t*>(message.data()),
                             message.size());
        };

        if (target.has_value()) {
            if (*target >= config_.num_hosts || *target == node_id_) {
                return false;
            }
            return sendTo(*target, DEMO_UNICAST);
        }

        bool success = true;
        bool has_peer = false;
        for (std::size_t id = 0; id < config_.num_hosts; ++id) {
            if (id == node_id_) {
                continue;
            }
            has_peer = true;
            if (!sendTo(static_cast<std::uint8_t>(id), DEMO_BROADCAST)) {
                success = false;
            }
        }
        return has_peer && success;
    }

private:
    // Reserva la región DSM: una vista PROT_NONE en dsm_base, la que usan las
    // aplicaciones, y un alias RW interno sobre el mismo memfd para que el nodo
    // pueda escribir páginas sin abrir la protección de la vista.
    void setupMemory() {
        if (pager::region_base.load() != 0) {
            throw std::runtime_error("Ya existe una región DSM en este proceso");
        }
        const std::size_t total_pages = page_table_.size();
        const std::size_t bytes = total_pages * config_.page_size;

        memfd_ = memfd_create("dsm", MFD_CLOEXEC);
        if (memfd_ < 0 || ftruncate(memfd_, static_cast<off_t>(bytes)) != 0) {
            throw std::runtime_error(std::string("memfd: ") + std::strerror(errno));
        }
        void* const wanted = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(config_.dsm_base));
        void* view = mmap(wanted, bytes, PROT_NONE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE, memfd_, 0);
        if (view == MAP_FAILED || view != wanted) {
            if (view != MAP_FAILED) {
                munmap(view, bytes);
            }
            throw std::runtime_error("No se pudo mapear la región DSM en dsm_base");
        }
        view_ = view;
        region_size_ = bytes;
        void* alias = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd_, 0);
        if (alias == MAP_FAILED) {
            throw std::runtime_error(std::string("mmap alias: ") + std::strerror(errno));
        }
        alias_ = static_cast<std::uint8_t*>(alias);

        if (pipe2(fault_pipe_.data(), O_CLOEXEC) != 0) {
            throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
        }
        fault_generations_ = std::make_unique<std::atomic<std::uint32_t>[]>(total_pages);
        fault_failed_ = std::make_unique<std::atomic<std::uint8_t>[]>(total_pages);

        pager::generations = fault_generations_.get();
        pager::failed = fault_failed_.get();
        pager::page_bytes = config_.page_size;
        pager::region_bytes = bytes;
        pager::fault_fd = fault_pipe_[1];
        struct sigaction action {};
        action.sa_sigaction = pager::onSegv;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, &pager::previous_action) != 0) {
            throw std::runtime_error(std::string("sigaction: ") + std::strerror(errno));
        }
        signal_installed_ = true;
        pager::region_base = reinterpret_cast<std::uintptr_t>(view_);
    }

    // Seguro de llamar varias veces y con la región a medio crear.
    void teardownMemory() {
        if (pager::region_base.load() == reinterpret_cast<std::uintptr_t>(view_)) {
            pager::region_base = 0;
        }
        if (fault_pipe_[1] >= 0) {
            pager::fault_fd = -1;
            close(fault_pipe_[1]);  // El worker recibe EOF y termina.
            fault_pipe_[1] = -1;
        }
        if (fault_thread_.joinable()) {
            fault_thread_.join();
        }
        if (signal_installed_) {
            pager::restorePreviousHandler();
            signal_installed_ = false;
        }
        if (fault_pipe_[0] >= 0) {
            close(fault_pipe_[0]);
            fault_pipe_[0] = -1;
        }
        if (alias_ != nullptr) {
            munmap(alias_, region_size_);
            alias_ = nullptr;
        }
        if (view_ != nullptr) {
            munmap(view_, region_size_);
            view_ = nullptr;
        }
        if (memfd_ >= 0) {
            close(memfd_);
            memfd_ = -1;
        }
    }

    void protectPage(std::uint32_t page, PageState state) {
        void* address = static_cast<std::uint8_t*>(view_) +
                        static_cast<std::size_t>(page) * config_.page_size;
        if (mprotect(address, config_.page_size, protectionFor(state)) != 0) {
            throw std::runtime_error(std::string("mprotect: ") + std::strerror(errno));
        }
    }

    // Un solo hilo atiende los fallos, así cada página se recupera una vez a la vez.
    void faultLoop() {
        pager::FaultMsg message{};
        while (true) {
            const ssize_t got = read(fault_pipe_[0], &message, sizeof(message));
            if (got == 0) {
                return;
            }
            if (got != static_cast<ssize_t>(sizeof(message))) {
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                return;
            }
            if (message.page >= page_table_.size()) {
                continue;
            }
            bool resolved = false;
            try {
                resolved = resolveFault(message.page, message.write != 0);
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(console_output_mutex);
                std::cerr << "dsm: " << error.what() << '\n';
            }
            fault_failed_[message.page] = resolved ? 0 : 1;
            fault_generations_[message.page].fetch_add(1);
        }
    }

    bool resolveFault(std::uint32_t page, bool write_access) {
        const std::uint32_t local_start =
            static_cast<std::uint32_t>(node_id_) * config_.pages_per_node;
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        PageEntry& entry = page_table_[page];
        const bool local_page = page >= local_start &&
                                page - local_start < config_.pages_per_node &&
                                entry.proc_id != 0;
        if (!local_page) {
            // shortcut: páginas remotas o sin asignar fallan; PAGE_REQUEST llega en el día 3.
            return false;
        }
        const auto current = static_cast<PageState>(entry.state);
        const MsiAccessPolicy policy = msiPolicyForAccess(
            current, write_access ? PageAccess::Write : PageAccess::Read);
        // El nodo es home y la única copia, así que no hay que pedir ni invalidar.
        // Si policy.retry_access, el reintento de la escritura sube S -> M.
        if (policy.granted_state != current) {
            protectPage(page, policy.granted_state);
            entry.state = static_cast<std::uint8_t>(policy.granted_state);
        }
        node_table_.recordPageState(node_id_, current, policy.granted_state, true);
        return true;
    }

    struct Connection {
        int fd;
        Channel channel;
        std::array<std::uint8_t, MESSAGE_HEADER_SIZE> header_bytes{};
        std::size_t header_received = 0;
        MsgHeader header{};
        std::vector<std::uint8_t> payload;
        std::size_t payload_received = 0;
        std::uint32_t peer_ipv4 = 0;
    };

    int openListener(Channel channel) const {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error("socket() falló al abrir un canal TCP");
        }

        int reuse_address = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                       sizeof(reuse_address)) < 0) {
            close(fd);
            throw std::runtime_error("setsockopt(SO_REUSEADDR) falló");
        }

        sockaddr_in local_address{};
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(channelPort(config_, node_id_, channel));
        if (bind(fd, reinterpret_cast<sockaddr*>(&local_address),
                 sizeof(local_address)) < 0) {
            const std::string error = std::strerror(errno);
            close(fd);
            throw std::runtime_error("No se pudo abrir el puerto " +
                                     std::to_string(ntohs(local_address.sin_port)) +
                                     ": " + error);
        }
        if (listen(fd, MAX_NODES) < 0) {
            const std::string error = std::strerror(errno);
            close(fd);
            throw std::runtime_error("listen() falló en el puerto " +
                                     std::to_string(ntohs(local_address.sin_port)) +
                                     ": " + error);
        }
        if (!setNonBlocking(fd)) {
            close(fd);
            throw std::runtime_error("No se pudo configurar el listener TCP como no bloqueante");
        }
        return fd;
    }

    static bool setNonBlocking(int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
    }

    void networkLoop() {
        std::vector<Connection> connections;
        while (running_) {
            std::vector<pollfd> descriptors;
            descriptors.reserve(connections.size() + CHANNEL_COUNT);
            for (const int listener : listeners_) {
                descriptors.push_back({listener, POLLIN, 0});
            }
            for (const auto& connection : connections) {
                descriptors.push_back({connection.fd, POLLIN, 0});
            }

            const int ready = poll(descriptors.data(), descriptors.size(), 250);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "poll() falló en los listeners TCP: "
                          << std::strerror(errno) << '\n';
                running_ = false;
                break;
            }

            const std::size_t polled_connections =
                descriptors.size() - CHANNEL_COUNT;
            for (std::size_t index = polled_connections; index > 0; --index) {
                const std::size_t connection_index = index - 1;
                const short events =
                    descriptors[CHANNEL_COUNT + connection_index].revents;
                if (events != 0 &&
                    !readMessage(connections[connection_index], events)) {
                    close(connections[connection_index].fd);
                    connections.erase(connections.begin() +
                                      static_cast<std::ptrdiff_t>(connection_index));
                }
            }

            if (ready > 0) {
                for (std::size_t role = 0; role < CHANNEL_COUNT; ++role) {
                    if ((descriptors[role].revents & POLLIN) != 0) {
                        acceptConnections(static_cast<Channel>(role), connections);
                    }
                }
            }
        }

        for (const auto& connection : connections) {
            close(connection.fd);
        }
    }

    void acceptConnections(Channel channel, std::vector<Connection>& connections) {
        while (running_) {
            if (connections.size() >= MAX_NODES * CHANNEL_COUNT) {
                return;
            }
            sockaddr_in peer_address{};
            socklen_t address_length = sizeof(peer_address);
            const int fd = accept(listeners_[static_cast<std::size_t>(channel)],
                                  reinterpret_cast<sockaddr*>(&peer_address),
                                  &address_length);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                std::cerr << "accept() falló en el listener TCP: "
                          << std::strerror(errno) << '\n';
                return;
            }
            if (!setNonBlocking(fd)) {
                close(fd);
                continue;
            }
            Connection connection{};
            connection.fd = fd;
            connection.channel = channel;
            connection.peer_ipv4 = peer_address.sin_addr.s_addr;
            connections.push_back(std::move(connection));
        }
    }

    bool readMessage(Connection& connection, short events) {
        if ((events & POLLNVAL) != 0) {
            return false;
        }
        const bool peer_closed = (events & (POLLERR | POLLHUP)) != 0;

        while (connection.header_received < connection.header_bytes.size()) {
            const ssize_t count = recv(
                connection.fd,
                connection.header_bytes.data() + connection.header_received,
                connection.header_bytes.size() - connection.header_received, 0);
            if (count > 0) {
                connection.header_received += static_cast<std::size_t>(count);
            } else if (count == 0) {
                return false;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return !peer_closed;
            } else {
                return false;
            }
        }

        if (connection.payload.empty() && connection.payload_received == 0) {
            if (!decodeHeader(connection.header_bytes, connection.header) ||
                connection.header.sender >= config_.num_hosts ||
                connection.header.sender == node_id_) {
                return false;
            }

            if (!validMessage(connection)) {
                return false;
            }
            try {
                connection.payload.resize(connection.header.payload_len);
            } catch (const std::bad_alloc&) {
                std::cerr << "No hay memoria para recibir el mensaje DSM\n";
                return false;
            }
        }

        while (connection.payload_received < connection.payload.size()) {
            const ssize_t count = recv(
                connection.fd,
                connection.payload.data() + connection.payload_received,
                connection.payload.size() - connection.payload_received, 0);
            if (count > 0) {
                connection.payload_received += static_cast<std::size_t>(count);
            } else if (count == 0) {
                return false;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return !peer_closed;
            } else {
                return false;
            }
        }

        if (!node_table_.recordContact(connection.header.sender,
                                       connection.peer_ipv4)) {
            return false;
        }

        handleMessage(connection);
        return false;
    }

    bool validMessage(const Connection& connection) const {
        const MsgHeader& header = connection.header;

        if (header.type == HEARTBEAT_MESSAGE) {
            return connection.channel == Channel::Heartbeat &&
                   header.status == STATUS_OK && header.flags == 0 &&
                   header.page == PAGE_NONE && header.payload_len == 0;
        }
        if (header.type == DEMO_MESSAGE) {
            return connection.channel != Channel::Heartbeat &&
                   header.status == STATUS_OK &&
                   (header.flags == DEMO_BROADCAST ||
                    header.flags == DEMO_UNICAST) &&
                   header.page == PAGE_NONE && header.payload_len > 0 &&
                   header.payload_len <= DEMO_MAX_BYTES &&
                   header.payload_len <= config_.page_size;
        }
        if (connection.channel != Channel::Control || header.flags != 0) {
            return false;
        }

        switch (static_cast<MessageType>(header.type)) {
        case MessageType::Join:
            return header.seq != 0 && header.status == STATUS_OK &&
                   header.page == PAGE_NONE &&
                   header.payload_len == 0;
        case MessageType::JoinAck:
            return header.seq != 0 && header.page == PAGE_NONE &&
                   header.payload_len == 0 &&
                   header.status <= STATUS_INVALID;
        case MessageType::AllocRequest:
            return header.seq != 0 && header.status == STATUS_OK &&
                   header.page == PAGE_NONE &&
                   header.payload_len == ALLOC_REQUEST_SIZE;
        case MessageType::AllocReply:
            return header.seq != 0 && header.page == PAGE_NONE &&
                   ((header.status == STATUS_OK &&
                     header.payload_len == PAGE_INDEX_SIZE) ||
                    (header.status > STATUS_OK &&
                     header.status <= STATUS_INVALID &&
                     header.payload_len == 0));
        case MessageType::Free:
            return header.seq != 0 && header.status == STATUS_OK &&
                   header.page == PAGE_NONE &&
                   header.payload_len == FREE_REQUEST_SIZE;
        case MessageType::FreeReply:
            return header.seq != 0 && header.page == PAGE_NONE &&
                   header.payload_len == 0 &&
                   header.status <= STATUS_INVALID;
        default:
            return false;
        }
    }

    void handleMessage(Connection& connection) {
        const MsgHeader& header = connection.header;
        if (header.type == HEARTBEAT_MESSAGE) {
            return;
        }
        if (header.type == DEMO_MESSAGE) {
            const std::string message(connection.payload.begin(),
                                      connection.payload.end());
            const char* channel_name =
                connection.channel == Channel::Data ? "data" : "control";
            const char* delivery =
                header.flags == DEMO_BROADCAST ? "broadcast" : "direct";
            std::lock_guard<std::mutex> lock(console_output_mutex);
            std::cout << "\nReceived " << delivery << " from node "
                      << static_cast<unsigned int>(header.sender)
                      << " on " << channel_name << ": " << message << '\n'
                      << "Comandos: nodes | broadcast <data|control> <mensaje> | "
                         "send <node_id> <data|control> <mensaje> | quit\n"
                      << std::flush;
            return;
        }

        switch (static_cast<MessageType>(header.type)) {
        case MessageType::Join: {
            MsgHeader reply{};
            reply.type = static_cast<std::uint8_t>(MessageType::JoinAck);
            reply.seq = header.seq;
            reply.page = PAGE_NONE;
            sendFrame(header.sender, Channel::Control, reply, nullptr, 0);
            break;
        }
        case MessageType::JoinAck:
        case MessageType::AllocReply:
        case MessageType::FreeReply:
            completePending(header, std::move(connection.payload));
            break;
        case MessageType::AllocRequest:
            handleAllocRequest(header, connection.payload);
            break;
        case MessageType::Free:
            handleFreeRequest(header, connection.payload);
            break;
        default:
            break;
        }
    }

    int allocateAtHome(std::uint32_t count, std::uint32_t proc_id,
                       std::uint32_t& first_page) {
        if (count == 0 || count > config_.pages_per_node || proc_id == 0) {
            return DSM_E_INVAL;
        }

        const std::uint32_t range_start =
            static_cast<std::uint32_t>(node_id_) * config_.pages_per_node;
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        std::uint32_t run_start = range_start;
        std::uint32_t run_length = 0;
        const std::uint32_t range_end = range_start + config_.pages_per_node;
        for (std::uint32_t page = range_start; page < range_end; ++page) {
            if (page_table_[page].proc_id == 0) {
                if (run_length == 0) {
                    run_start = page;
                }
                if (++run_length == count) {
                    try {
                        const auto inserted = home_allocations_.emplace(
                            run_start, Allocation{run_start, count, proc_id});
                        if (!inserted.second) {
                            return DSM_E_NET;
                        }
                    } catch (const std::bad_alloc&) {
                        return DSM_E_NOMEM;
                    }
                    for (std::uint32_t offset = 0; offset < count; ++offset) {
                        PageEntry& entry = page_table_[run_start + offset];
                        entry.proc_id = proc_id;
                        entry.copyset = std::uint32_t{1} << node_id_;
                        entry.order = offset;
                        entry.state = static_cast<std::uint8_t>(PageState::Invalid);
                        entry.owner = node_id_;
                        entry.reserved = 0;
                    }
                    node_table_.recordAllocation(node_id_, count);
                    first_page = run_start;
                    return DSM_OK;
                }
            } else {
                run_length = 0;
            }
        }
        return DSM_E_NOMEM;
    }

    int releaseAtHome(std::uint32_t first_page, std::uint32_t count,
                      std::uint32_t proc_id) {
        const std::uint32_t range_start =
            static_cast<std::uint32_t>(node_id_) * config_.pages_per_node;
        if (first_page < range_start || count == 0 ||
            first_page - range_start > config_.pages_per_node ||
            count > config_.pages_per_node - (first_page - range_start)) {
            return DSM_E_BADADDR;
        }

        std::lock_guard<std::mutex> lock(page_table_mutex_);
        const auto allocation = home_allocations_.find(first_page);
        if (allocation == home_allocations_.end() ||
            allocation->second.num_pages != count ||
            allocation->second.proc_id != proc_id) {
            return DSM_E_BADADDR;
        }
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            const PageEntry& entry = page_table_[first_page + offset];
            if (entry.proc_id != proc_id || entry.order != offset) {
                return DSM_E_BADADDR;
            }
        }
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            const auto state =
                static_cast<PageState>(page_table_[first_page + offset].state);
            if (view_ != nullptr) {
                protectPage(first_page + offset, PageState::Invalid);
                // Una página reutilizada no debe exponer datos del proceso anterior.
                std::memset(alias_ + static_cast<std::size_t>(first_page + offset) *
                                         config_.page_size,
                            0, config_.page_size);
            }
            node_table_.recordPageState(node_id_, state, PageState::Invalid, false);
            page_table_[first_page + offset] = PageEntry{};
            page_table_[first_page + offset].state =
                static_cast<std::uint8_t>(PageState::Invalid);
            page_table_[first_page + offset].owner = NODE_NONE;
        }
        home_allocations_.erase(allocation);
        node_table_.recordFree(node_id_, count);
        return DSM_OK;
    }

    void handleAllocRequest(const MsgHeader& request,
                            const std::vector<std::uint8_t>& payload) {
        std::uint32_t count = 0;
        std::uint32_t proc_id = 0;
        std::uint32_t first_page = 0;
        std::uint8_t status = STATUS_INVALID;
        if (readU32(payload, 0, count) && readU32(payload, 4, proc_id)) {
            const int result = allocateAtHome(count, proc_id, first_page);
            status = result == DSM_OK ? STATUS_OK
                   : result == DSM_E_NOMEM ? STATUS_NO_SPACE
                                           : STATUS_INVALID;
        }

        std::array<std::uint8_t, PAGE_INDEX_SIZE> response{};
        std::size_t response_size = 0;
        if (status == STATUS_OK) {
            response[0] = static_cast<std::uint8_t>((first_page >> 24) & 0xFF);
            response[1] = static_cast<std::uint8_t>((first_page >> 16) & 0xFF);
            response[2] = static_cast<std::uint8_t>((first_page >> 8) & 0xFF);
            response[3] = static_cast<std::uint8_t>(first_page & 0xFF);
            response_size = response.size();
        }
        MsgHeader reply{};
        reply.type = static_cast<std::uint8_t>(MessageType::AllocReply);
        reply.status = status;
        reply.seq = request.seq;
        reply.page = PAGE_NONE;
        if (!sendFrame(request.sender, Channel::Control, reply,
                       response_size == 0 ? nullptr : response.data(),
                       response_size) &&
            status == STATUS_OK) {
            releaseAtHome(first_page, count, proc_id);
        }
    }

    void handleFreeRequest(const MsgHeader& request,
                           const std::vector<std::uint8_t>& payload) {
        std::uint32_t first_page = 0;
        std::uint32_t count = 0;
        std::uint32_t proc_id = 0;
        std::uint8_t status = STATUS_INVALID;
        if (readU32(payload, 0, first_page) &&
            readU32(payload, 4, count) &&
            readU32(payload, 8, proc_id) &&
            releaseAtHome(first_page, count, proc_id) == DSM_OK) {
            status = STATUS_OK;
        }

        MsgHeader reply{};
        reply.type = static_cast<std::uint8_t>(MessageType::FreeReply);
        reply.status = status;
        reply.seq = request.seq;
        reply.page = PAGE_NONE;
        sendFrame(request.sender, Channel::Control, reply, nullptr, 0);
    }

    void completePending(const MsgHeader& response,
                         std::vector<std::uint8_t>&& payload) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto found = pending_.find(response.seq);
        if (found == pending_.end()) {
            return;
        }
        const std::shared_ptr<Pending>& pending = found->second;
        if (pending->dest != response.sender ||
            pending->response_type != response.type) {
            return;
        }
        pending->status = response.status;
        pending->payload = std::move(payload);
        pending->result = response.status == STATUS_OK ? DSM_OK
            : response.type == static_cast<std::uint8_t>(MessageType::AllocReply) &&
                      response.status == STATUS_NO_SPACE
                ? DSM_E_NOMEM
                : response.type == static_cast<std::uint8_t>(MessageType::FreeReply)
                    ? DSM_E_BADADDR
                    : DSM_E_NET;
        pending->complete = true;
        pending->ready.notify_one();
    }

    void failPendingRequests() {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& entry : pending_) {
            entry.second->result = DSM_E_NET;
            entry.second->complete = true;
            entry.second->ready.notify_one();
        }
    }

    int requestMessage(std::uint8_t target, MessageType type, std::uint32_t page,
                       const std::vector<std::uint8_t>& payload,
                       MessageType response_type,
                       std::vector<std::uint8_t>& response_payload) {
        if (target >= config_.num_hosts || target == node_id_ || !running_) {
            return DSM_E_NODE_DOWN;
        }

        auto pending = std::make_shared<Pending>();
        pending->dest = target;
        pending->response_type = static_cast<std::uint8_t>(response_type);
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_.size() >= MAX_PENDING) {
                return DSM_E_NOMEM;
            }
            for (std::size_t attempt = 0; attempt <= MAX_PENDING; ++attempt) {
                pending->seq = next_sequence_.fetch_add(1);
                if (pending->seq != 0 && pending_.count(pending->seq) == 0) {
                    break;
                }
            }
            if (pending->seq == 0 || pending_.count(pending->seq) != 0) {
                return DSM_E_NOMEM;
            }
            pending_.emplace(pending->seq, pending);
        }

        MsgHeader request{};
        request.type = static_cast<std::uint8_t>(type);
        request.seq = pending->seq;
        request.page = page;
        const bool sent = sendFrame(
            target, Channel::Control, request,
            payload.empty() ? nullptr : payload.data(), payload.size());
        std::unique_lock<std::mutex> lock(pending_mutex_);
        if (!sent) {
            pending_.erase(pending->seq);
            return DSM_E_NODE_DOWN;
        }
        const bool completed = pending->ready.wait_for(
            lock, std::chrono::milliseconds(config_.timeout_ms),
            [&] { return pending->complete || !running_; });
        const auto found = pending_.find(pending->seq);
        if (found != pending_.end()) {
            pending_.erase(found);
        }
        if (!completed || !pending->complete) {
            return running_ ? DSM_E_TIMEOUT : DSM_E_NET;
        }
        response_payload = std::move(pending->payload);
        return pending->result;
    }

    int sendFreeRequest(std::uint8_t home, std::uint32_t first_page,
                        std::uint32_t count, std::uint32_t proc_id) {
        std::vector<std::uint8_t> request;
        request.reserve(FREE_REQUEST_SIZE);
        appendU32(request, first_page);
        appendU32(request, count);
        appendU32(request, proc_id);
        std::vector<std::uint8_t> response;
        return requestMessage(home, MessageType::Free, PAGE_NONE, request,
                              MessageType::FreeReply, response);
    }

    void joinLoop() {
        while (running_) {
            for (std::size_t id = 0; id < config_.num_hosts && running_; ++id) {
                const auto peer = static_cast<std::uint8_t>(id);
                if (peer == node_id_ || joined_[id]) {
                    continue;
                }
                std::vector<std::uint8_t> response;
                const int result = requestMessage(
                    peer, MessageType::Join, PAGE_NONE, {},
                    MessageType::JoinAck, response);
                if (result == DSM_OK) {
                    joined_[id] = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    bool sendFrame(std::uint8_t target_id, Channel channel, MsgHeader header,
                   const std::uint8_t* payload, std::size_t payload_size) const {
        if (target_id >= config_.num_hosts ||
            payload_size > UINT32_MAX ||
            (payload_size != 0 && payload == nullptr)) {
            return false;
        }
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || !setNonBlocking(fd)) {
            if (fd >= 0) {
                close(fd);
            }
            return false;
        }

        sockaddr_in destination = peer_addresses_[target_id];
        destination.sin_port = htons(channelPort(config_, target_id, channel));
        const int connect_result = connect(fd,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        if (connect_result < 0 && errno != EINPROGRESS) {
            close(fd);
            return false;
        }
        if (connect_result < 0) {
            pollfd descriptor{fd, POLLOUT, 0};
            if (poll(&descriptor, 1, 150) <= 0 ||
                (descriptor.revents & POLLOUT) == 0) {
                close(fd);
                return false;
            }
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                           &error_length) < 0 || socket_error != 0) {
                close(fd);
                return false;
            }
        }

        header.sender = node_id_;
        header.payload_len = static_cast<std::uint32_t>(payload_size);
        const auto encoded_header = encodeHeader(header);
        std::vector<std::uint8_t> bytes;
        try {
            bytes.assign(encoded_header.begin(), encoded_header.end());
            bytes.reserve(MESSAGE_HEADER_SIZE + payload_size);
            if (payload_size > 0) {
                bytes.insert(bytes.end(), payload, payload + payload_size);
            }
        } catch (const std::bad_alloc&) {
            close(fd);
            return false;
        }

        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t count = send(fd, bytes.data() + sent,
                                       bytes.size() - sent, MSG_NOSIGNAL);
            if (count > 0) {
                sent += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd descriptor{fd, POLLOUT, 0};
                if (poll(&descriptor, 1, 150) > 0 &&
                    (descriptor.revents & POLLOUT) != 0) {
                    continue;
                }
            }
            close(fd);
            return false;
        }

        close(fd);
        return true;
    }

    bool sendHeartbeat(std::uint8_t target_id) const {
        MsgHeader header{};
        header.type = HEARTBEAT_MESSAGE;
        header.page = PAGE_NONE;
        return sendFrame(target_id, Channel::Heartbeat, header, nullptr, 0);
    }

    void heartbeatLoop() {
        while (running_) {
            for (std::size_t id = 0; id < config_.num_hosts && running_; ++id) {
                if (id != node_id_) {
                    sendHeartbeat(static_cast<std::uint8_t>(id));
                }
            }
            node_table_.markTimedOut(std::chrono::milliseconds(config_.timeout_ms));
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_ms));
        }
    }

    static void joinThreads(std::vector<std::thread>& threads, std::mutex& mutex) {
        std::vector<std::thread> pending_threads;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_threads.swap(threads);
        }
        for (auto& thread : pending_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    Config config_;
    const std::uint8_t node_id_;
    NodeTable node_table_;
    mutable std::mutex page_table_mutex_;
    std::vector<PageEntry> page_table_;
    std::array<sockaddr_in, MAX_NODES> peer_addresses_{};
    std::atomic<bool> running_{false};
    std::array<int, CHANNEL_COUNT> listeners_{{-1, -1, -1}};
    mutable std::atomic<std::uint32_t> next_sequence_{1};

    std::thread network_thread_;
    std::thread heartbeat_thread_;
    std::thread join_thread_;
    std::vector<std::thread> user_threads_;
    std::mutex user_threads_mutex_;
    std::array<bool, MAX_NODES> joined_{};
    std::mutex pending_mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<Pending>> pending_;
    std::mutex allocations_mutex_;
    std::unordered_map<std::uintptr_t, Allocation> allocations_;
    std::unordered_map<std::uint32_t, Allocation> home_allocations_;
    std::atomic<std::size_t> allocation_cursor_{0};

    // Región DSM local (ver setupMemory).
    int memfd_ = -1;
    void* view_ = nullptr;
    std::uint8_t* alias_ = nullptr;
    std::size_t region_size_ = 0;
    std::array<int, 2> fault_pipe_{{-1, -1}};
    std::thread fault_thread_;
    bool signal_installed_ = false;
    std::unique_ptr<std::atomic<std::uint32_t>[]> fault_generations_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> fault_failed_;
};

void printNodes(const Node& node, std::mutex& output_mutex) {
    const auto entries = node.nodes();
    const Config& config = node.config();
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "\nNodos configurados (" << entries.size() << "):\n"
              << "ID  Estado   Host                 Datos  Control  Heartbeat\n";
    for (const auto& entry : entries) {
        const char* state =
            entry.state == static_cast<std::uint8_t>(NodeState::Online)
                ? "EN LINEA"
                : "CAIDO";
        std::cout << static_cast<unsigned int>(entry.node_id) << "   "
                  << state << "  "
                  << config.hosts[entry.node_id] << "  "
                  << channelPort(config, entry.node_id, Channel::Data) << "  "
                  << channelPort(config, entry.node_id, Channel::Control) << "  "
                  << channelPort(config, entry.node_id, Channel::Heartbeat) << '\n';
    }
    std::cout << "Comandos: nodes | broadcast <data|control> <mensaje> | "
                 "send <node_id> <data|control> <mensaje> | quit\n"
              << std::flush;
}

std::uint8_t parseNodeId(const std::string& value) {
    const std::uint64_t node_id = parseUnsigned(value, "node_id");
    if (node_id >= MAX_NODES) {
        throw std::runtime_error("node_id debe estar entre 0 y 31");
    }
    return static_cast<std::uint8_t>(node_id);
}

bool handleDemoCommand(const Node& node, const std::string& command) {
    std::istringstream input(command);
    std::string operation;
    input >> operation;
    if (operation != "broadcast" && operation != "send") {
        return false;
    }

    std::optional<std::uint8_t> target;
    if (operation == "send") {
        std::string target_text;
        if (!(input >> target_text)) {
            std::lock_guard<std::mutex> lock(console_output_mutex);
            std::cout << "Uso: send <node_id> <data|control> <mensaje>\n";
            return true;
        }
        try {
            target = parseNodeId(target_text);
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(console_output_mutex);
            std::cout << "Destino inválido: " << error.what() << '\n';
            return true;
        }
    }

    std::string channel_text;
    if (!(input >> channel_text)) {
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cout << "Falta el canal: use data o control.\n";
        return true;
    }

    Channel channel;
    if (channel_text == "data") {
        channel = Channel::Data;
    } else if (channel_text == "control") {
        channel = Channel::Control;
    } else {
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cout << "Canal inválido: use data o control.\n";
        return true;
    }

    std::string message;
    std::getline(input, message);
    message = trim(message);
    if (message.empty()) {
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cout << "El mensaje no puede estar vacío.\n";
        return true;
    }
    if (message.size() > DEMO_MAX_BYTES) {
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cout << "El mensaje supera el límite de "
                  << DEMO_MAX_BYTES << " bytes.\n";
        return true;
    }

    if (node.sendDemoMessage(target, channel, message)) {
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cout << "Mensaje enviado por " << channel_text
                  << (target ? " al nodo " +
                                   std::to_string(static_cast<unsigned int>(*target))
                             : " a los demás nodos")
                  << ".\n" << std::flush;
    } else {
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cout << "No se pudo enviar el mensaje. Verifique el destino, "
                     "que haya pares configurados y que el puerto esté accesible.\n"
                  << std::flush;
    }
    return true;
}

} // namespace dsm

namespace {

std::mutex api_mutex;
std::unique_ptr<dsm::Node> api_node;

}

extern "C" int dsm_init(const char* config_path, int node_id) {
    if (config_path == nullptr || node_id < 0 ||
        node_id >= static_cast<int>(dsm::MAX_NODES)) {
        return DSM_E_INVAL;
    }

    std::lock_guard<std::mutex> lock(api_mutex);
    if (api_node) {
        return DSM_E_INVAL;
    }

    dsm::Config config;
    try {
        config = dsm::loadConfig(config_path);
    } catch (const std::bad_alloc&) {
        return DSM_E_NOMEM;
    } catch (const std::exception& error) {
        std::cerr << "dsm_init: " << error.what() << '\n';
        return DSM_E_CONFIG;
    }
    if (node_id >= config.num_hosts) {
        return DSM_E_INVAL;
    }

    try {
        auto node = std::make_unique<dsm::Node>(
            config, static_cast<std::uint8_t>(node_id));
        node->start();
        api_node = std::move(node);
    } catch (const std::bad_alloc&) {
        return DSM_E_NOMEM;
    } catch (const std::exception& error) {
        std::cerr << "dsm_init: " << error.what() << '\n';
        return DSM_E_NET;
    }
    return DSM_OK;
}

extern "C" void dsm_shutdown(void) {
    std::lock_guard<std::mutex> lock(api_mutex);
    api_node.reset();
}

extern "C" int dsm_alloc(size_t size, uint16_t proc, void** ptr) {
    if (ptr == nullptr) {
        return DSM_E_INVAL;
    }
    *ptr = nullptr;
    if (size == 0 || proc == 0) {
        return DSM_E_INVAL;
    }

    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return DSM_E_INVAL;
    }
    try {
        return api_node->allocate(size, proc, ptr);
    } catch (const std::bad_alloc&) {
        return DSM_E_NOMEM;
    } catch (const std::exception& error) {
        std::cerr << "dsm_alloc: " << error.what() << '\n';
        return DSM_E_NET;
    }
}

extern "C" int dsm_free(void* ptr) {
    if (ptr == nullptr) {
        return DSM_E_BADADDR;
    }
    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return DSM_E_INVAL;
    }
    try {
        return api_node->release(ptr);
    } catch (const std::bad_alloc&) {
        return DSM_E_NOMEM;
    } catch (const std::exception& error) {
        std::cerr << "dsm_free: " << error.what() << '\n';
        return DSM_E_NET;
    }
}

extern "C" int dsm_nodes(dsm_node_t* out, int max) {
    if (out == nullptr || max <= 0) {
        return DSM_E_INVAL;
    }
    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return DSM_E_INVAL;
    }

    const auto nodes = api_node->nodes();
    const std::size_t copied =
        std::min(nodes.size(), static_cast<std::size_t>(max));
    for (std::size_t index = 0; index < copied; ++index) {
        const auto& source = nodes[index];
        out[index] = {source.ipv4, source.first_page, source.num_pages,
                      source.node_id, source.state, source.reserved};
    }
    return static_cast<int>(copied);
}

extern "C" int dsm_stats(int node_id, dsm_stats_t* out) {
    if (out == nullptr || node_id < 0 ||
        node_id >= static_cast<int>(dsm::MAX_NODES)) {
        return DSM_E_INVAL;
    }
    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return DSM_E_INVAL;
    }

    const auto nodes = api_node->nodes();
    if (static_cast<std::size_t>(node_id) >= nodes.size()) {
        return DSM_E_INVAL;
    }
    if (node_id != api_node->nodeId()) {
        return nodes[static_cast<std::size_t>(node_id)].state ==
                       static_cast<std::uint8_t>(dsm::NodeState::Online)
                   ? DSM_E_NOT_IMPLEMENTED
                   : DSM_E_NODE_DOWN;
    }

    const auto stats = api_node->stats();
    const auto& source = stats[static_cast<std::size_t>(node_id)];
    *out = {source.total_pages, source.free_pages, source.pages_m, source.pages_s,
            source.pages_i, source.faults, source.pages_sent, source.pages_recv};
    return DSM_OK;
}

extern "C" int dsm_page_table(int node_id, uint32_t first_page, uint32_t count,
                               dsm_page_t* out) {
    if (out == nullptr || node_id < 0 ||
        node_id >= static_cast<int>(dsm::MAX_NODES) || count == 0 ||
        count > dsm::QUERY_MAX) {
        return DSM_E_INVAL;
    }
    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return DSM_E_INVAL;
    }

    const auto nodes = api_node->nodes();
    if (static_cast<std::size_t>(node_id) >= nodes.size()) {
        return DSM_E_INVAL;
    }
    if (node_id != api_node->nodeId()) {
        return nodes[static_cast<std::size_t>(node_id)].state ==
                       static_cast<std::uint8_t>(dsm::NodeState::Online)
                   ? DSM_E_NOT_IMPLEMENTED
                   : DSM_E_NODE_DOWN;
    }

    std::vector<dsm::PageEntry> pages(count);
    if (!api_node->copyPageTable(static_cast<std::uint8_t>(node_id), first_page,
                                 count, pages.data())) {
        return DSM_E_INVAL;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& source = pages[index];
        out[index] = {source.proc_id, source.copyset, source.order,
                      source.state, source.owner, source.reserved};
    }
    return static_cast<int>(count);
}

extern "C" size_t dsm_page_size(void) {
    std::lock_guard<std::mutex> lock(api_mutex);
    return api_node ? api_node->config().page_size : 0;
}

extern "C" const char* dsm_strerror(int err) {
    switch (err) {
    case DSM_OK: return "Success";
    case DSM_E_INVAL: return "Invalid argument or DSM node not initialized";
    case DSM_E_CONFIG: return "Invalid DSM configuration";
    case DSM_E_NOMEM: return "Out of memory";
    case DSM_E_BADADDR: return "Invalid DSM address";
    case DSM_E_NODE_DOWN: return "DSM node is offline";
    case DSM_E_TIMEOUT: return "DSM operation timed out";
    case DSM_E_NET: return "DSM network initialization failed";
    case DSM_E_NOT_IMPLEMENTED:
        return "Operation is not implemented";
    default: return "Unknown DSM error";
    }
}

#ifndef DSM_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " <ruta-dsm.conf> <node_id>\n";
        return 2;
    }

    try {
        const dsm::Config config = dsm::loadConfig(argv[1]);
        const std::uint8_t node_id = dsm::parseNodeId(argv[2]);
        dsm::Node node(config, node_id);
        node.start();

        std::atomic<bool> monitor_running{true};
        std::thread monitor([&] {
            auto previous = node.nodes();
            while (monitor_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const auto current = node.nodes();
                bool changed = current.size() != previous.size();
                for (std::size_t id = 0; !changed && id < current.size(); ++id) {
                    changed = current[id].state != previous[id].state;
                }
                if (changed) {
                    dsm::printNodes(node, dsm::console_output_mutex);
                    previous = current;
                }
            }
        });

        dsm::printNodes(node, dsm::console_output_mutex);
        std::string command;
        while (std::getline(std::cin, command)) {
            command = dsm::trim(command);
            if (command == "quit" || command == "exit") {
                break;
            }
            if (command == "nodes") {
                dsm::printNodes(node, dsm::console_output_mutex);
            } else if (dsm::handleDemoCommand(node, command)) {
                continue;
            } else if (!command.empty()) {
                std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
                std::cout << "Comando desconocido. Use: nodes | broadcast "
                             "<data|control> <mensaje> | send <node_id> "
                             "<data|control> <mensaje> | quit\n"
                          << std::flush;
            }
        }

        monitor_running = false;
        monitor.join();
        node.stop();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
#endif
