// Tipos, constantes y declaraciones compartidas por los módulos del DSM.
#ifndef DSM_INTERNAL_H
#define DSM_INTERNAL_H

#include "dsm.h"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
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
constexpr std::uint32_t QUERY_STATS = 0;
constexpr std::uint32_t QUERY_PAGES = 1;
constexpr std::size_t QUERY_REQUEST_SIZE = 12;
constexpr std::size_t QUERY_STATS_SIZE = 32;
constexpr std::size_t QUERY_PAGE_ENTRY_SIZE = 16;
constexpr std::uint8_t REQUEST_NONE = 0;
constexpr std::uint8_t REQUEST_ACTIVE = 1;
constexpr std::uint8_t REQUEST_INVALIDATED = 2;

constexpr std::uint32_t nodeBit(std::uint8_t node_id) {
    return std::uint32_t{1} << node_id;
}
extern std::mutex console_output_mutex;
// Registro de eventos (mensajes recibidos, cambios de estado de nodos). Solo se
// imprimen en vivo dentro del comando monitor, para no cortar lo que se escribe.
void monitorEvent(const std::string& text);
void setMonitorLive(bool live);

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

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value);

bool readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t& value);

std::array<std::uint8_t, MESSAGE_HEADER_SIZE> encodeHeader(const MsgHeader& header);

bool decodeHeader(const std::array<std::uint8_t, MESSAGE_HEADER_SIZE>& bytes,
                  MsgHeader& header);

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
        // Para el nodo local, M/S/I cuentan sus copias de todas las páginas del sistema.
        stats_[local_node_id].pages_i =
            static_cast<std::uint32_t>(node_count) * pages_per_node;
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

    void recordTransfer(std::uint8_t node_id, bool sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++(sent ? stats_[node_id].pages_sent : stats_[node_id].pages_recv);
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

std::string trim(const std::string& value);

std::uint64_t parseUnsigned(const std::string& value, const std::string& key);

Config loadConfig(const std::string& path);

std::uint32_t resolveIPv4(const char* host);

bool isLocalIPv4(std::uint32_t ipv4);

std::uint16_t heartbeatPort(const Config& config, std::uint8_t node_id);

std::uint16_t channelPort(const Config& config, std::uint8_t node_id,
                          Channel channel);

// Estado global que lee el manejador de SIGSEGV. Solo hay una región DSM por
// proceso, porque un proceso solo puede tener un manejador de SIGSEGV.
namespace pager {

struct FaultMsg {
    std::uint32_t page;
    std::uint32_t write;
};

extern std::atomic<std::uintptr_t> region_base;
extern std::atomic<std::size_t> region_bytes;
extern std::atomic<std::size_t> page_bytes;
extern std::atomic<int> fault_fd;
// Por página: el worker incrementa la generación al terminar cada solicitud.
extern std::atomic<std::uint32_t>* generations;
extern std::atomic<std::uint8_t>* failed;
extern struct sigaction previous_action;

void restorePreviousHandler();

// Solo usa operaciones async-signal-safe: atómicos lock-free, write y nanosleep.
void onSegv(int, siginfo_t* info, void* context);

}  // namespace pager

int protectionFor(PageState state);

class Node {
public:
    Node(Config config, std::uint8_t node_id);

    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    void start();

    void stop();

    void stopService();

    void launchUserThread(std::function<void()> application);
    // Espera a que terminen las aplicaciones lanzadas con launchUserThread.
    void waitUserThreads();

    std::vector<NodeInfo> nodes() const;

    std::vector<NodeStats> stats() const;

    int queryStats(std::uint8_t target, NodeStats& out);

    int queryPageTable(std::uint8_t target, std::uint32_t first_page,
                       std::uint32_t count, std::vector<PageEntry>& out);

    bool copyPageTable(std::uint8_t target_node, std::uint32_t first_page,
                       std::uint32_t count, PageEntry* out) const;

    const Config& config() const;

    std::uint8_t nodeId() const;

    int allocate(std::size_t size, std::uint32_t proc_id, void** out);

    int release(void* ptr);

    bool sendDemoMessage(std::optional<std::uint8_t> target, Channel channel,
                         const std::string& message) const;

private:
    void setupMemory();

    void teardownMemory();

    void protectPage(std::uint32_t page, PageState state);

    void faultLoop();

    std::uint8_t homeOf(std::uint32_t page) const;

    std::uint8_t* aliasPage(std::uint32_t page) const;

    void setLocalState(std::uint32_t page, PageState next, bool fault);

    bool resolveFault(std::uint32_t page, bool write_access);

    int grantPage(std::uint32_t page, std::uint8_t requester, bool write_access,
                  std::vector<std::uint8_t>* data);

    void revokeCopy(std::uint32_t page, std::uint8_t peer);

    int remoteQuery(std::uint8_t target, std::uint32_t kind, std::uint32_t first_page,
                    std::uint32_t count, std::vector<std::uint8_t>& reply);

    void handleQuery(const MsgHeader& request, const std::vector<std::uint8_t>& payload);

    void handleInvalidate(const MsgHeader& request);

    void handlePageRequest(const MsgHeader& request,
                           const std::vector<std::uint8_t>& payload);

    void serviceLoop();

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

    int openListener(Channel channel) const;

    static bool setNonBlocking(int fd);

    void networkLoop();

    void acceptConnections(Channel channel, std::vector<Connection>& connections);

    bool readMessage(Connection& connection, short events);

    bool validMessage(const Connection& connection) const;

    void handleMessage(Connection& connection);

    int allocateAtHome(std::uint32_t count, std::uint32_t proc_id,
                       std::uint32_t& first_page);

    int releaseAtHome(std::uint32_t first_page, std::uint32_t count,
                      std::uint32_t proc_id);

    void handleAllocRequest(const MsgHeader& request,
                            const std::vector<std::uint8_t>& payload);

    void handleFreeRequest(const MsgHeader& request,
                           const std::vector<std::uint8_t>& payload);

    void completePending(const MsgHeader& response,
                         std::vector<std::uint8_t>&& payload);

    void failPendingRequests();

    int requestMessage(std::uint8_t target, MessageType type, std::uint32_t page,
                       const std::vector<std::uint8_t>& payload,
                       MessageType response_type,
                       std::vector<std::uint8_t>& response_payload);

    int sendFreeRequest(std::uint8_t home, std::uint32_t first_page,
                        std::uint32_t count, std::uint32_t proc_id);

    void joinLoop();

    bool sendFrame(std::uint8_t target_id, Channel channel, MsgHeader header,
                   const std::uint8_t* payload, std::size_t payload_size) const;

    bool sendHeartbeat(std::uint8_t target_id) const;

    void heartbeatLoop();

    static void joinThreads(std::vector<std::thread>& threads, std::mutex& mutex);

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

    // Directorio de las páginas home: serializa grantPage y releaseAtHome.
    std::mutex directory_mutex_;
    // Por página: si el hilo de fallos espera PAGE_DATA (REQUEST_*).
    std::vector<std::uint8_t> request_state_;
    struct ServiceJob {
        MsgHeader header;
        std::vector<std::uint8_t> payload;
    };
    std::mutex service_mutex_;
    std::condition_variable service_ready_;
    std::deque<ServiceJob> service_queue_;
    std::thread service_thread_;

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

// Nodo creado por dsm_init en este proceso (nulo si no hay); lo usa la consola.
Node* apiNode();

}  // namespace dsm

#endif
