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

void Node::setRunHandler(std::function<void(std::uint8_t, const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(run_handler_mutex_);
    run_handler_ = std::move(handler);
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

// Pide una parte del segmento a un home (o la reserva aquí si el home es este nodo).
int Node::requestPiece(std::uint8_t home, std::uint32_t count, std::uint32_t proc_id,
                       std::uint32_t exact_first, std::uint32_t order_base,
                       std::uint32_t& first_page, std::uint32_t& piece) {
    if (home == node_id_) {
        return allocateAtHome(count, proc_id, node_id_, exact_first, order_base, first_page,
                              piece);
    }
    std::vector<std::uint8_t> request;
    appendU32(request, count);
    appendU32(request, proc_id);
    if (exact_first != PAGE_NONE) {
        appendU32(request, exact_first);
        appendU32(request, order_base);
    }
    std::vector<std::uint8_t> response;
    const int result = requestMessage(home, MessageType::AllocRequest, PAGE_NONE, request,
                                      MessageType::AllocReply, response);
    if (result == DSM_OK &&
        (!readU32(response, 0, first_page) || !readU32(response, 4, piece))) {
        return DSM_E_NET;
    }
    return result;
}

// Suelta la parte [first_page, first_page + count) en su home.
int Node::releasePiece(std::uint32_t first_page, std::uint32_t count, std::uint32_t proc_id) {
    const std::uint8_t home = homeOf(first_page);
    return home == node_id_ ? releaseAtHome(first_page, count, proc_id, node_id_)
                            : sendFreeRequest(home, first_page, count, proc_id);
}

int Node::allocate(std::size_t size, std::uint32_t proc_id, void** out) {
    if (size == 0 || proc_id == 0 || out == nullptr) {
        return DSM_E_INVAL;
    }
    const std::size_t page_size = config_.page_size;
    const std::size_t page_count = size / page_size + (size % page_size != 0 ? 1 : 0);
    if (page_count == 0) {
        return DSM_E_INVAL;
    }
    if (page_count > page_table_.size()) {
        return DSM_E_NOMEM;  // Más que la memoria de todo el sistema.
    }
    const auto total = static_cast<std::uint32_t>(page_count);

    // Segmento compartido: el mismo proc da la misma región en cualquier nodo. Su home
    // ancla es siempre proc % nodos, así todos le piden al mismo nodo y ninguno tiene
    // que crearlo primero. Si el ancla no tiene lugar para todo, toma las páginas libres
    // del final de su rango y el resto se pide a los nodos siguientes, cuyos rangos
    // siguen en la numeración global: la dirección del segmento queda continua.
    const auto anchor = static_cast<std::uint8_t>(proc_id % config_.num_hosts);
    const auto node_entries = nodes();
    const auto online = [&](std::uint8_t id) {
        return id == node_id_ ||
               node_entries[id].state == static_cast<std::uint8_t>(NodeState::Online);
    };
    if (!online(anchor)) {
        return DSM_E_NODE_DOWN;
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> pieces;  // (primera página, cantidad)
    const auto rollback = [&] {
        for (const auto& piece : pieces) {
            releasePiece(piece.first, piece.second, proc_id);
        }
    };
    std::uint32_t first_page = 0;
    std::uint32_t done = 0;
    int result = requestPiece(anchor, total, proc_id, PAGE_NONE, 0, first_page, done);
    if (result != DSM_OK) {
        return result;
    }
    pieces.emplace_back(first_page, done);
    for (std::size_t home = anchor + 1U; done < total; ++home) {
        if (home >= config_.num_hosts || !online(static_cast<std::uint8_t>(home))) {
            rollback();
            return DSM_E_NOMEM;
        }
        const std::uint32_t count = std::min(total - done, config_.pages_per_node);
        const auto range_start = static_cast<std::uint32_t>(home) * config_.pages_per_node;
        std::uint32_t piece_first = 0;
        std::uint32_t piece = 0;
        result = requestPiece(static_cast<std::uint8_t>(home), count, proc_id, range_start,
                              done, piece_first, piece);
        if (result != DSM_OK) {
            rollback();
            return result;
        }
        pieces.emplace_back(piece_first, piece);
        done += piece;
    }

    // La región entera ya está mapeada en dsm_base, así que la dirección es válida.
    void* allocation_ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
        config_.dsm_base + static_cast<std::uint64_t>(first_page) * config_.page_size));
    try {
        std::lock_guard<std::mutex> lock(allocations_mutex_);
        // Si ya estaba, este nodo se había unido antes: es la misma referencia.
        allocations_.emplace(reinterpret_cast<std::uintptr_t>(allocation_ptr),
                             Allocation{first_page, total, proc_id});
    } catch (const std::bad_alloc&) {
        rollback();
        throw;
    }
    *out = allocation_ptr;
    return DSM_OK;
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

    // El segmento puede ocupar varios nodos: se suelta la parte de cada home.
    int result = DSM_OK;
    const std::uint32_t end = allocation.first_page + allocation.num_pages;
    for (std::uint32_t page = allocation.first_page; page < end;) {
        const std::uint32_t range_end = (page / config_.pages_per_node + 1) *
                                        config_.pages_per_node;
        const std::uint32_t count = std::min(end, range_end) - page;
        const int piece_result = releasePiece(page, count, allocation.proc_id);
        if (piece_result != DSM_OK && result == DSM_OK) {
            result = piece_result;
        }
        page += count;
    }
    if (result == DSM_OK) {
        std::lock_guard<std::mutex> lock(allocations_mutex_);
        allocations_.erase(address);
    }
    return result;
}

// Reserva, o une al solicitante a, la parte de un segmento que cae en el rango de
// este nodo. Con exact_first == PAGE_NONE este nodo es el ancla (proc % nodos): busca
// el segmento del proceso; si no existe lo crea entero si cabe y, si no, toma las
// páginas libres del final de su rango (piece dice cuántas) para que el resto siga en
// el nodo siguiente. Con exact_first es una parte de continuación: count páginas desde
// el inicio del rango, con órdenes desde order_base.
int Node::allocateAtHome(std::uint32_t count, std::uint32_t proc_id, std::uint8_t requester,
                         std::uint32_t exact_first, std::uint32_t order_base,
                         std::uint32_t& first_page, std::uint32_t& piece) {
    const std::uint32_t range_start =
        static_cast<std::uint32_t>(node_id_) * config_.pages_per_node;
    const std::uint32_t range_end = range_start + config_.pages_per_node;
    const bool continuation = exact_first != PAGE_NONE;
    if (count == 0 || proc_id == 0 ||
        (continuation && (exact_first != range_start || count > config_.pages_per_node))) {
        return DSM_E_INVAL;
    }

    std::lock_guard<std::mutex> lock(page_table_mutex_);
    // Si la parte ya existe, el nodo se une. Buscar y crear bajo el mismo mutex evita
    // dos segmentos si dos nodos piden a la vez.
    for (auto& entry : home_allocations_) {
        Allocation& existing = entry.second;
        if (existing.proc_id != proc_id) {
            continue;
        }
        if ((continuation ? existing.num_pages : existing.total) != count ||
            (continuation && existing.first_page != exact_first)) {
            return DSM_E_INVAL;
        }
        existing.nodes |= nodeBit(requester);
        first_page = existing.first_page;
        piece = existing.num_pages;
        recordLog(LOG_JOIN, first_page, requester, 0, proc_id, NODE_NONE);
        return DSM_OK;
    }

    std::uint32_t start = PAGE_NONE;
    std::uint32_t length = 0;
    if (continuation) {
        for (std::uint32_t page = range_start; page < range_start + count; ++page) {
            if (page_table_[page].proc_id != 0) {
                return DSM_E_NOMEM;
            }
        }
        start = range_start;
        length = count;
    } else {
        std::uint32_t run = 0;
        for (std::uint32_t page = range_start; page < range_end && count <= config_.pages_per_node;
             ++page) {
            run = page_table_[page].proc_id == 0 ? run + 1 : 0;
            if (run == count) {
                start = page + 1 - count;
                length = count;
                break;
            }
        }
        if (start == PAGE_NONE) {
            // No cabe entero: toma el final libre del rango si hay un nodo siguiente.
            std::uint32_t tail = 0;
            while (tail < config_.pages_per_node &&
                   page_table_[range_end - 1 - tail].proc_id == 0) {
                ++tail;
            }
            if (tail == 0 || node_id_ + 1U >= config_.num_hosts) {
                return DSM_E_NOMEM;
            }
            start = range_end - tail;
            length = tail;
        }
    }

    try {
        Allocation allocation{start, length, proc_id, nodeBit(requester)};
        allocation.total = continuation ? length : count;
        if (!home_allocations_.emplace(start, allocation).second) {
            return DSM_E_NET;
        }
    } catch (const std::bad_alloc&) {
        return DSM_E_NOMEM;
    }
    for (std::uint32_t offset = 0; offset < length; ++offset) {
        PageEntry& entry = page_table_[start + offset];
        entry.proc_id = proc_id;
        entry.copyset = nodeBit(node_id_);
        entry.order = order_base + offset;
        entry.state = static_cast<std::uint8_t>(PageState::Invalid);
        entry.owner = node_id_;
        entry.reserved = 0;
    }
    node_table_.recordAllocation(node_id_, length);
    first_page = start;
    piece = length;
    recordLog(LOG_CREATE, first_page, requester, 0, proc_id, NODE_NONE);
    return DSM_OK;
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
            // Las órdenes siguen la posición en el segmento completo.
            if (entry.proc_id != proc_id ||
                entry.order != page_table_[first_page].order + offset) {
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
