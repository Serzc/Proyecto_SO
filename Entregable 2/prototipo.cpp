#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dsm {

constexpr std::size_t MAX_NODES = 32;
constexpr std::size_t HOME_LOCKS = 64;

enum class NodeState : std::uint8_t {
    Offline = 0,
    Online = 1
};

enum class ServiceOperation {
    PageRequest,
    Free
};

struct NodeInfo {
    std::uint32_t ipv4;       // Dirección IPv4 en orden de red.
    std::uint32_t first_page; // node_id * pages_per_node.
    std::uint32_t num_pages;
    std::uint8_t node_id;
    std::uint8_t state;
    std::uint16_t reserved;
};
static_assert(sizeof(NodeInfo) == 16, "NodeInfo debe ocupar 16 bytes");

class NodeTable {
public:
    using Clock = std::chrono::steady_clock;

    explicit NodeTable(std::size_t node_count) : node_count_(node_count) {
        if (node_count == 0 || node_count > MAX_NODES) {
            throw std::invalid_argument("node_count debe estar entre 1 y MAX_NODES");
        }

        for (std::size_t id = 0; id < node_count_; ++id) {
            entries_[id].node_id = static_cast<std::uint8_t>(id);
            entries_[id].state = static_cast<std::uint8_t>(NodeState::Offline);
            configured_[id] = true;
        }
    }

    bool registerNode(const NodeInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (info.node_id >= node_count_ || !configured_[info.node_id]) {
            return false;
        }

        entries_[info.node_id] = info;
        entries_[info.node_id].state = static_cast<std::uint8_t>(NodeState::Online);
        last_seen_[info.node_id] = Clock::now();
        return true;
    }

    bool recordHeartbeat(std::uint8_t node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node_id >= node_count_ || !configured_[node_id]) {
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
            if (configured_[id] &&
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

private:
    const std::size_t node_count_;
    mutable std::mutex mutex_; // Protege entradas y marcas de última actividad.
    std::array<NodeInfo, MAX_NODES> entries_{};
    std::array<Clock::time_point, MAX_NODES> last_seen_{};
    std::array<bool, MAX_NODES> configured_{};
};

struct NodeConfig {
    std::uint64_t dsm_base = 0x100000000000ULL;
    std::uint32_t page_size = 4096;
    std::uint32_t pages_per_node = 256;
    std::uint32_t heartbeat_ms = 1000;
    std::uint32_t timeout_ms = 5000;
    std::uint16_t port_base = 28400;
    std::uint16_t num_hosts = 1;
    std::uint8_t reserved[4]{};
    char hosts[MAX_NODES][64]{};
};
static_assert(sizeof(NodeConfig) == 2080, "NodeConfig debe ocupar 2080 bytes");

class Node {
public:
    Node(NodeConfig config, std::uint8_t node_id)
        : config_(config), node_id_(node_id), node_table_(config.num_hosts) {
        if (node_id_ >= config_.num_hosts) {
            throw std::invalid_argument("node_id no pertenece a la configuración");
        }
        if (config_.page_size == 0 || config_.page_size % 4096 != 0) {
            throw std::invalid_argument("page_size debe ser múltiplo de 4096");
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

        network_thread_ = std::thread(&Node::networkLoop, this);
        heartbeat_thread_ = std::thread(&Node::heartbeatLoop, this);
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }

        joinThreads(service_threads_, service_threads_mutex_);
        joinThreads(user_threads_, user_threads_mutex_);
    }

    // El hilo principal de consola puede iniciar una aplicación DSM como hilo de usuario.
    void launchUserThread(std::function<void()> application) {
        if (!running_) {
            throw std::logic_error("El nodo debe iniciarse antes de lanzar una aplicación");
        }

        std::lock_guard<std::mutex> lock(user_threads_mutex_);
        user_threads_.emplace_back(std::move(application));
    }

    NodeTable& nodeTable() {
        return node_table_;
    }

private:
    void networkLoop() {
        while (running_) {
            // TODO: poll() sobre los puertos de datos, control y heartbeat,
            // aceptar conexiones y despachar mensajes sin bloquear el listener.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void heartbeatLoop() {
        while (running_) {
            // TODO: enviar HEARTBEAT a los demás nodos por el canal dedicado.
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
        // TODO: atender PAGE_REQUEST o FREE como home, incluyendo esperas
        // de respuestas de otros nodos fuera del hilo de red.
        (void)operation;
        (void)requester;
    }

    static void joinThreads(std::vector<std::thread>& threads, std::mutex& mutex) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads.clear();
    }

    NodeConfig config_;
    const std::uint8_t node_id_;
    NodeTable node_table_;
    std::atomic<bool> running_{false};

    std::thread network_thread_;
    std::thread heartbeat_thread_;
    std::vector<std::thread> service_threads_;
    std::vector<std::thread> user_threads_;
    std::mutex service_threads_mutex_;
    std::mutex user_threads_mutex_;
    std::array<std::mutex, HOME_LOCKS> home_locks_;
};

} // namespace dsm
