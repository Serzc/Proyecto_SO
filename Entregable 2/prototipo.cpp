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
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <semaphore.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

//g++ -std=c++17 -pthread -Wall -Wextra -Wpedantic "Entregable 2/prototipo.cpp" -o dsmnode

namespace dsm {

constexpr std::size_t MAX_NODES = 32;
constexpr std::size_t MAX_PENDING = 256;
constexpr std::size_t HOME_LOCKS = 64;
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
    QueryReply = 12
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
    sem_t sem;
    void* out;
    std::uint32_t seq;
    std::int32_t status;
    std::uint32_t out_cap;
    std::uint32_t out_len;
    std::uint8_t dest;
    std::uint8_t reserved[7];
};
static_assert(sizeof(Pending) == 64, "Pending debe ocupar 64 bytes en Linux x86-64");

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

enum class ServiceOperation {
    PageRequest,
    Free
};

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
            stats_[id].pages_i = static_cast<std::uint32_t>(node_count) *
                                 pages_per_node;
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

    bool recordHeartbeat(std::uint8_t node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_ || node_id == local_node_id_) {
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
            for (std::size_t role = 0; role < CHANNEL_COUNT; ++role) {
                listeners_[role] = openListener(static_cast<Channel>(role));
            }
            node_table_.markLocalOnline();
            network_thread_ = std::thread(&Node::networkLoop, this);
            heartbeat_thread_ = std::thread(&Node::heartbeatLoop, this);
        } catch (...) {
            running_ = false;
            if (network_thread_.joinable()) {
                network_thread_.join();
            }
            for (int& listener : listeners_) {
                if (listener >= 0) {
                    close(listener);
                    listener = -1;
                }
            }
            throw;
        }
    }

    void stop() {
        running_ = false;
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        for (int& listener : listeners_) {
            if (listener >= 0) {
                close(listener);
                listener = -1;
            }
        }

        joinThreads(service_threads_, service_threads_mutex_);
        joinThreads(user_threads_, user_threads_mutex_);
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
            std::memcpy(&connection.header, connection.header_bytes.data(),
                        sizeof(connection.header));
            connection.header.seq = ntohl(connection.header.seq);
            connection.header.page = ntohl(connection.header.page);
            connection.header.payload_len = ntohl(connection.header.payload_len);

            const bool heartbeat =
                connection.header.type == HEARTBEAT_MESSAGE &&
                connection.channel == Channel::Heartbeat &&
                connection.header.status == 0 && connection.header.flags == 0 &&
                connection.header.page == PAGE_NONE &&
                connection.header.payload_len == 0;
            const bool demo =
                connection.header.type == DEMO_MESSAGE &&
                connection.channel != Channel::Heartbeat &&
                connection.header.status == 0 &&
                (connection.header.flags == DEMO_BROADCAST ||
                 connection.header.flags == DEMO_UNICAST) &&
                connection.header.page == PAGE_NONE &&
                connection.header.payload_len > 0 &&
                connection.header.payload_len <= DEMO_MAX_BYTES &&
                connection.header.payload_len <= config_.page_size;
            if ((!heartbeat && !demo) ||
                connection.header.sender >= config_.num_hosts ||
                connection.header.sender == node_id_) {
                return false;
            }
            connection.payload.resize(connection.header.payload_len);
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

        const bool valid_peer =
            connection.header.type == HEARTBEAT_MESSAGE
                ? node_table_.recordHeartbeat(connection.header.sender)
                : node_table_.recordContact(connection.header.sender,
                                            connection.peer_ipv4);
        if (!valid_peer) {
            return false;
        }

        if (connection.header.type == DEMO_MESSAGE) {
            const std::string message(connection.payload.begin(),
                                      connection.payload.end());
            const char* channel_name =
                connection.channel == Channel::Data ? "data" : "control";
            const char* delivery =
                connection.header.flags == DEMO_BROADCAST ? "broadcast" : "direct";
            std::lock_guard<std::mutex> lock(console_output_mutex);
            std::cout << "\nReceived " << delivery << " from node "
                      << static_cast<unsigned int>(connection.header.sender)
                      << " on " << channel_name << ": " << message << '\n'
                      << "Comandos: nodes | broadcast <data|control> <mensaje> | "
                         "send <node_id> <data|control> <mensaje> | quit\n"
                      << std::flush;
        }
        return false;
    }

    bool sendFrame(std::uint8_t target_id, Channel channel, MsgHeader header,
                   const std::uint8_t* payload, std::size_t payload_size) const {
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
        header.seq = htonl(header.seq);
        header.page = htonl(header.page);
        header.payload_len = htonl(static_cast<std::uint32_t>(payload_size));
        std::vector<std::uint8_t> bytes(sizeof(header) + payload_size);
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (payload_size > 0) {
            std::memcpy(bytes.data() + sizeof(header), payload, payload_size);
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

    void dispatchService(ServiceOperation operation, std::uint32_t page,
                         std::uint8_t requester) {
        std::lock_guard<std::mutex> lock(service_threads_mutex_);
        service_threads_.emplace_back(
            &Node::serviceLoop, this, operation, page, requester);
    }

    void serviceLoop(ServiceOperation operation, std::uint32_t page,
                     std::uint8_t requester) {
        std::lock_guard<std::mutex> page_lock(home_locks_[page % HOME_LOCKS]);
        (void)operation;
        (void)requester;
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
    std::vector<std::thread> service_threads_;
    std::vector<std::thread> user_threads_;
    std::mutex service_threads_mutex_;
    std::mutex user_threads_mutex_;
    std::array<std::mutex, HOME_LOCKS> home_locks_;
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
    const std::size_t page_size = api_node->config().page_size;
    const std::size_t pages = size / page_size + (size % page_size != 0);
    if (pages > api_node->config().pages_per_node) {
        return DSM_E_INVAL;
    }
    return DSM_E_NOT_IMPLEMENTED;
}

extern "C" int dsm_free(void* ptr) {
    if (ptr == nullptr) {
        return DSM_E_BADADDR;
    }
    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return DSM_E_INVAL;
    }
    return DSM_E_NOT_IMPLEMENTED;
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
        return "Operation requires DSM page allocation or query protocol, not implemented yet";
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
