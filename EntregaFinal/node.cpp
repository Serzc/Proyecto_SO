// Ciclo de vida del nodo, asignación en el home y consultas de estado.
#include "dsm_internal.h"

namespace dsm {

std::mutex console_output_mutex;

namespace {

constexpr std::size_t MONITOR_HISTORY = 50;
std::deque<std::string> monitor_events;  // Últimos eventos, protegidos por console_output_mutex.
bool monitor_live = false;

}  // namespace

void monitorEvent(const std::string& text) {
    char stamp[16] = "";
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) != nullptr) {
        std::strftime(stamp, sizeof(stamp), "[%H:%M:%S] ", &local);
    }
    std::lock_guard<std::mutex> lock(console_output_mutex);
    monitor_events.push_back(stamp + text);
    if (monitor_events.size() > MONITOR_HISTORY) {
        monitor_events.pop_front();
    }
    if (monitor_live) {
        std::cout << monitor_events.back() << '\n' << std::flush;
    }
}

bool monitorIsLive() {
    std::lock_guard<std::mutex> lock(console_output_mutex);
    return monitor_live;
}

void setMonitorLive(bool live) {
    std::lock_guard<std::mutex> lock(console_output_mutex);
    monitor_live = live;
    if (!live) {
        return;
    }
    std::cout << "Eventos recientes:\n";
    if (monitor_events.empty()) {
        std::cout << "(ninguno)\n";
    }
    for (const std::string& event : monitor_events) {
        std::cout << event << '\n';
    }
    std::cout << "--- en vivo; presione Enter para salir ---\n" << std::flush;
}

Node::Node(Config config, std::uint8_t node_id)
    : config_(config), node_id_(node_id),
      node_table_(config.num_hosts, node_id, config.pages_per_node) {
    sem_init(&directory_sem_, 0, 1);
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
    request_state_.assign(total_pages, REQUEST_NONE);
    for (auto& page : page_table_) {
        page.state = static_cast<std::uint8_t>(PageState::Invalid);
        page.owner = NODE_NONE;
    }
}

Node::~Node() {
    stop();
    sem_destroy(&directory_sem_);
}

void Node::start() {
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
        service_thread_ = std::thread(&Node::serviceLoop, this);
    } catch (...) {
        running_ = false;
        failPendingRequests();
        stopService();
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

void Node::stop() {
    running_ = false;
    failPendingRequests();
    stopService();
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

void Node::stopService() {
    {
        std::lock_guard<std::mutex> lock(service_mutex_);
        service_ready_.notify_all();
    }
    if (service_thread_.joinable()) {
        service_thread_.join();
    }
}

void Node::waitUserThreads() {
    joinThreads(user_threads_, user_threads_mutex_);
}

void Node::launchUserThread(std::function<void()> application) {
    if (!running_) {
        throw std::logic_error("El nodo debe iniciarse antes de lanzar una aplicación");
    }
    std::lock_guard<std::mutex> lock(user_threads_mutex_);
    user_threads_.emplace_back(std::move(application));
}

std::vector<NodeInfo> Node::nodes() const {
    return node_table_.snapshot();
}

std::vector<NodeStats> Node::stats() const {
    return node_table_.statsSnapshot();
}

// Estadísticas de cualquier nodo: las locales se leen directo, las remotas
// con QUERY_REQUEST.
int Node::queryStats(std::uint8_t target, NodeStats& out) {
    if (target == node_id_) {
        out = stats()[node_id_];
        return DSM_OK;
    }
    std::vector<std::uint8_t> reply;
    const int result = remoteQuery(target, QUERY_STATS, 0, 0, reply);
    if (result != DSM_OK) {
        return result;
    }
    std::uint32_t* fields[] = {&out.total_pages, &out.free_pages, &out.pages_m,
                               &out.pages_s, &out.pages_i, &out.faults,
                               &out.pages_sent, &out.pages_recv};
    for (std::size_t index = 0; index < 8; ++index) {
        if (!readU32(reply, index * 4, *fields[index])) {
            return DSM_E_NET;
        }
    }
    return DSM_OK;
}

int Node::queryPageTable(std::uint8_t target, std::uint32_t first_page,
                   std::uint32_t count, std::vector<PageEntry>& out) {
    out.assign(count, PageEntry{});
    if (target == node_id_) {
        return copyPageTable(target, first_page, count, out.data()) ? DSM_OK
                                                                    : DSM_E_INVAL;
    }
    std::vector<std::uint8_t> reply;
    const int result = remoteQuery(target, QUERY_PAGES, first_page, count, reply);
    if (result != DSM_OK) {
        return result;
    }
    if (reply.size() != count * QUERY_PAGE_ENTRY_SIZE) {
        return DSM_E_NET;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t at = index * QUERY_PAGE_ENTRY_SIZE;
        PageEntry& entry = out[index];
        readU32(reply, at, entry.proc_id);
        readU32(reply, at + 4, entry.copyset);
        readU32(reply, at + 8, entry.order);
        entry.state = reply[at + 12];
        entry.owner = reply[at + 13];
    }
    return DSM_OK;
}

bool Node::copyPageTable(std::uint8_t target_node, std::uint32_t first_page,
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

const Config& Node::config() const {
    return config_;
}

std::uint8_t Node::nodeId() const {
    return node_id_;
}

// Anota en el log del home una solicitud atendida (ver LogEntry).
void Node::recordLog(LogKind kind, std::uint32_t page, std::uint8_t node,
                     std::uint8_t result, std::uint32_t value, std::uint8_t extra) {
    LogEntry entry{};
    entry.time = static_cast<std::uint32_t>(std::time(nullptr));
    entry.page = page;
    entry.value = value;
    entry.kind = static_cast<std::uint8_t>(kind);
    entry.node = node;
    entry.result = result;
    entry.extra = extra;
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_.push_back(entry);
    if (log_.size() > LOG_CAPACITY) {
        log_.pop_front();
    }
}

std::vector<LogEntry> Node::localLog() const {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return std::vector<LogEntry>(log_.begin(), log_.end());
}

// Log de solicitudes de cualquier nodo: el local se lee directo, el remoto con QUERY_LOG.
int Node::queryLog(std::uint8_t target, std::vector<LogEntry>& out) {
    if (target == node_id_) {
        out = localLog();
        return DSM_OK;
    }
    std::vector<std::uint8_t> reply;
    const int result = remoteQuery(target, QUERY_LOG, 0, 0, reply);
    if (result != DSM_OK) {
        return result;
    }
    if (reply.size() % LOG_ENTRY_SIZE != 0) {
        return DSM_E_NET;
    }
    out.assign(reply.size() / LOG_ENTRY_SIZE, LogEntry{});
    for (std::size_t index = 0; index < out.size(); ++index) {
        const std::size_t at = index * LOG_ENTRY_SIZE;
        LogEntry& entry = out[index];
        readU32(reply, at, entry.time);
        readU32(reply, at + 4, entry.page);
        readU32(reply, at + 8, entry.value);
        entry.kind = reply[at + 12];
        entry.node = reply[at + 13];
        entry.result = reply[at + 14];
        entry.extra = reply[at + 15];
    }
    return DSM_OK;
}

int Node::allocate(std::size_t size, std::uint32_t proc_id, void** out) {
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

    // Segmento compartido: el mismo proc da la misma región en cualquier nodo. Su
    // home es siempre proc % nodos, así todos los nodos le piden al mismo home y
    // ninguno tiene que crearla primero.
    // shortcut: si ese home está lleno o caído no se intenta otro; usar otro proc.
    const auto shared_home = static_cast<std::uint8_t>(proc_id % config_.num_hosts);
    if (shared_home != node_id_ &&
        nodes()[shared_home].state != static_cast<std::uint8_t>(NodeState::Online)) {
        return DSM_E_NODE_DOWN;
    }
    const std::uint8_t candidates[] = {shared_home};

    int last_error = DSM_E_NOMEM;
    for (const std::uint8_t home : candidates) {
        std::uint32_t first_page = 0;
        int result = DSM_OK;
        if (home == node_id_) {
            result = allocateAtHome(static_cast<std::uint32_t>(page_count),
                                    proc_id, node_id_, first_page);
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
                                     proc_id, node_id_);
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
            // Si ya estaba, este nodo se había unido antes: es la misma referencia.
            static_cast<void>(inserted);
        } catch (const std::bad_alloc&) {
            if (home == node_id_) {
                releaseAtHome(first_page, static_cast<std::uint32_t>(page_count),
                              proc_id, node_id_);
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

int Node::release(void* ptr) {
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
                        allocation.proc_id, node_id_)
        : sendFreeRequest(home, allocation.first_page,
                          allocation.num_pages, allocation.proc_id);
    if (result == DSM_OK) {
        std::lock_guard<std::mutex> lock(allocations_mutex_);
        allocations_.erase(address);
    }
    return result;
}

int Node::allocateAtHome(std::uint32_t count, std::uint32_t proc_id,
                         std::uint8_t requester, std::uint32_t& first_page) {
    if (count == 0 || count > config_.pages_per_node || proc_id == 0) {
        return DSM_E_INVAL;
    }

    const std::uint32_t range_start =
        static_cast<std::uint32_t>(node_id_) * config_.pages_per_node;
    std::lock_guard<std::mutex> lock(page_table_mutex_);
    // Si el segmento del proceso ya existe, el nodo se une a él. Buscar y crear
    // bajo el mismo mutex evita dos segmentos si dos nodos piden a la vez.
    for (auto& entry : home_allocations_) {
        Allocation& existing = entry.second;
        if (existing.proc_id == proc_id) {
            if (existing.num_pages != count) {
                return DSM_E_INVAL;
            }
            existing.nodes |= nodeBit(requester);
            first_page = existing.first_page;
            recordLog(LOG_JOIN, first_page, requester, 0, proc_id, NODE_NONE);
            return DSM_OK;
        }
    }
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
                        run_start,
                        Allocation{run_start, count, proc_id, nodeBit(requester)});
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
                recordLog(LOG_CREATE, first_page, requester, 0, proc_id, NODE_NONE);
                return DSM_OK;
            }
        } else {
            run_length = 0;
        }
    }
    return DSM_E_NOMEM;
}

int Node::releaseAtHome(std::uint32_t first_page, std::uint32_t count,
                        std::uint32_t proc_id, std::uint8_t requester) {
    const std::uint32_t range_start =
        static_cast<std::uint32_t>(node_id_) * config_.pages_per_node;
    if (first_page < range_start || count == 0 ||
        first_page - range_start > config_.pages_per_node ||
        count > config_.pages_per_node - (first_page - range_start)) {
        return DSM_E_BADADDR;
    }

    // Las copias remotas se invalidan antes de liberar; por eso esta función
    // puede esperar a la red y no se llama desde el hilo de red (salvo el
    // rollback de handleAllocRequest, cuyas páginas no tienen copias remotas).
    SemaphoreLock directory(directory_sem_);
    std::vector<std::uint32_t> copysets(count);
    {
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        const auto allocation = home_allocations_.find(first_page);
        if (allocation == home_allocations_.end() ||
            allocation->second.num_pages != count ||
            allocation->second.proc_id != proc_id ||
            (allocation->second.nodes & nodeBit(requester)) == 0) {
            return DSM_E_BADADDR;
        }
        // El segmento solo se libera cuando lo suelta el último nodo unido, así
        // ningún nodo pierde memoria que todavía está leyendo.
        allocation->second.nodes &= ~nodeBit(requester);
        if (allocation->second.nodes != 0) {
            recordLog(LOG_LEAVE, first_page, requester, 0, proc_id, NODE_NONE);
            return DSM_OK;
        }
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            const PageEntry& entry = page_table_[first_page + offset];
            if (entry.proc_id != proc_id || entry.order != offset) {
                return DSM_E_BADADDR;
            }
            copysets[offset] = entry.copyset | nodeBit(entry.owner);
        }
    }
    for (std::uint32_t offset = 0; offset < count; ++offset) {
        for (std::size_t id = 0; id < config_.num_hosts; ++id) {
            const auto peer = static_cast<std::uint8_t>(id);
            if (peer != node_id_ && (copysets[offset] & nodeBit(peer)) != 0) {
                revokeCopy(first_page + offset, peer);
            }
        }
    }

    std::lock_guard<std::mutex> lock(page_table_mutex_);
    const auto allocation = home_allocations_.find(first_page);
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
    recordLog(LOG_RELEASE, first_page, requester, 0, proc_id, NODE_NONE);
    return DSM_OK;
}

void Node::joinThreads(std::vector<std::thread>& threads, std::mutex& mutex) {
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

}  // namespace dsm
