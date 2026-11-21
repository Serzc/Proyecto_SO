// Paginación: región DSM con memfd/mmap, mprotect, manejador de SIGSEGV,
// hilo de fallos y directorio MSI del home.
#include "dsm_internal.h"

namespace dsm {

namespace pager {

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

// Reserva la región DSM: una vista PROT_NONE en dsm_base, la que usan las
// aplicaciones, y un alias RW interno sobre el mismo memfd para que el nodo
// pueda escribir páginas sin abrir la protección de la vista.
void Node::setupMemory() {
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
void Node::teardownMemory() {
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

void Node::protectPage(std::uint32_t page, PageState state) {
    void* address = static_cast<std::uint8_t*>(view_) +
                    static_cast<std::size_t>(page) * config_.page_size;
    if (mprotect(address, config_.page_size, protectionFor(state)) != 0) {
        throw std::runtime_error(std::string("mprotect: ") + std::strerror(errno));
    }
}

// Un solo hilo atiende los fallos, así cada página se recupera una vez a la vez.
void Node::faultLoop() {
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

std::uint8_t Node::homeOf(std::uint32_t page) const {
    return static_cast<std::uint8_t>(page / config_.pages_per_node);
}

std::uint8_t* Node::aliasPage(std::uint32_t page) const {
    return alias_ + static_cast<std::size_t>(page) * config_.page_size;
}

// Cambia el estado MSI de la copia local. Requiere page_table_mutex_.
void Node::setLocalState(std::uint32_t page, PageState next, bool fault) {
    PageEntry& entry = page_table_[page];
    const auto current = static_cast<PageState>(entry.state);
    if (next != current) {
        protectPage(page, next);
        entry.state = static_cast<std::uint8_t>(next);
    }
    node_table_.recordPageState(node_id_, current, next, fault);
}

bool Node::resolveFault(std::uint32_t page, bool write_access) {
    const std::uint8_t home = homeOf(page);
    if (home == node_id_) {
        return grantPage(page, node_id_, write_access, nullptr) == DSM_OK;
    }

    {
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        request_state_[page] = REQUEST_ACTIVE;
    }
    std::vector<std::uint8_t> request;
    appendU32(request, write_access ? 1U : 0U);
    std::vector<std::uint8_t> data;
    const int result = requestMessage(home, MessageType::PageRequest, page,
                                      request, MessageType::PageData, data);

    std::lock_guard<std::mutex> lock(page_table_mutex_);
    const bool invalidated = request_state_[page] == REQUEST_INVALIDATED;
    request_state_[page] = REQUEST_NONE;
    if (result != DSM_OK || data.size() != config_.page_size) {
        return false;
    }
    if (invalidated) {
        // Llegó un INVALIDATE mientras se esperaba la página: la copia puede
        // ser vieja. Se descarta y el reintento del acceso pide otra.
        return true;
    }
    std::memcpy(aliasPage(page), data.data(), config_.page_size);
    node_table_.recordTransfer(node_id_, false);
    setLocalState(page, write_access ? PageState::Modified : PageState::Shared, true);
    return true;
}

// Lado home: actualiza el directorio de la página para que `requester`
// obtenga S (lectura) o M (escritura). Si data no es nulo, deja ahí el
// contenido vigente para enviarlo. Puede esperar respuestas de la red, por eso
// nunca se llama desde el hilo de red.
int Node::grantPage(std::uint32_t page, std::uint8_t requester, bool write_access,
              std::vector<std::uint8_t>* data) {
    std::lock_guard<std::mutex> directory(directory_mutex_);
    std::uint32_t copyset = 0;
    std::uint8_t owner = NODE_NONE;
    {
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        const PageEntry& entry = page_table_[page];
        if (entry.proc_id == 0) {
            return DSM_E_BADADDR;
        }
        copyset = entry.copyset;
        owner = entry.owner;
    }

    // Si otro nodo tiene la página en M, se recupera su contenido antes.
    // Si el dueño es quien pide, perdió su copia sin escribirla (ver
    // REQUEST_INVALIDATED) y la copia del home sigue vigente.
    if (owner != node_id_ && owner != requester) {
        revokeCopy(page, owner);
    }
    if (owner != node_id_) {
        copyset &= ~nodeBit(owner);
        owner = node_id_;
    }

    if (write_access) {
        for (std::size_t id = 0; id < config_.num_hosts; ++id) {
            const auto peer = static_cast<std::uint8_t>(id);
            if (peer == requester || (copyset & nodeBit(peer)) == 0) {
                continue;
            }
            if (peer == node_id_) {
                std::lock_guard<std::mutex> lock(page_table_mutex_);
                setLocalState(page, PageState::Invalid, false);
            } else {
                revokeCopy(page, peer);
            }
        }
        copyset = nodeBit(requester);
        owner = requester;
    } else {
        copyset |= nodeBit(requester);
    }

    std::lock_guard<std::mutex> lock(page_table_mutex_);
    PageEntry& entry = page_table_[page];
    entry.copyset = copyset;
    entry.owner = owner;
    if (requester == node_id_) {
        setLocalState(page, write_access ? PageState::Modified : PageState::Shared, true);
    } else {
        if (!write_access && static_cast<PageState>(entry.state) == PageState::Modified) {
            // El home deja de ser escritor único: baja a S para ver la copia compartida.
            setLocalState(page, PageState::Shared, false);
        }
        if (data != nullptr) {
            data->assign(aliasPage(page), aliasPage(page) + config_.page_size);
            node_table_.recordTransfer(node_id_, true);
        }
    }
    return DSM_OK;
}

// Invalida la copia de `peer`; si la tenía en M, guarda su contenido en el home.
void Node::revokeCopy(std::uint32_t page, std::uint8_t peer) {
    std::vector<std::uint8_t> data;
    const int result = requestMessage(peer, MessageType::Invalidate, page, {},
                                      MessageType::InvalidateAck, data);
    if (result != DSM_OK) {
        // shortcut: un nodo que no responde se da por caído y sin copia; si solo
        // estaba lento, la coherencia se rompe. Mejorar con reintentos.
        std::lock_guard<std::mutex> lock(console_output_mutex);
        std::cerr << "dsm: nodo " << static_cast<unsigned int>(peer)
                  << " no confirmó INVALIDATE de la página " << page
                  << " (" << result << ")\n";
        return;
    }
    if (data.size() == config_.page_size) {
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        std::memcpy(aliasPage(page), data.data(), config_.page_size);
        node_table_.recordTransfer(node_id_, false);
    }
}

// Hilo de red: responde de inmediato, sin esperar a otros nodos.
void Node::handleInvalidate(const MsgHeader& request) {
    std::vector<std::uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(page_table_mutex_);
        if (request_state_[request.page] == REQUEST_ACTIVE) {
            request_state_[request.page] = REQUEST_INVALIDATED;
        }
        const auto state = static_cast<PageState>(page_table_[request.page].state);
        setLocalState(request.page, PageState::Invalid, false);
        if (state == PageState::Modified) {
            // La protección ya es PROT_NONE: nadie escribe mientras se copia.
            data.assign(aliasPage(request.page),
                        aliasPage(request.page) + config_.page_size);
            node_table_.recordTransfer(node_id_, true);
        }
    }
    MsgHeader reply{};
    reply.type = static_cast<std::uint8_t>(MessageType::InvalidateAck);
    reply.seq = request.seq;
    reply.page = request.page;
    sendFrame(request.sender, Channel::Control, reply,
              data.empty() ? nullptr : data.data(), data.size());
}

void Node::handlePageRequest(const MsgHeader& request,
                       const std::vector<std::uint8_t>& payload) {
    std::uint32_t write_access = 0;
    std::vector<std::uint8_t> data;
    const int result = readU32(payload, 0, write_access) && write_access <= 1
        ? grantPage(request.page, request.sender, write_access == 1, &data)
        : DSM_E_INVAL;
    MsgHeader reply{};
    reply.type = static_cast<std::uint8_t>(MessageType::PageData);
    reply.status = result == DSM_OK ? STATUS_OK : STATUS_INVALID;
    reply.seq = request.seq;
    reply.page = request.page;
    sendFrame(request.sender, Channel::Control, reply,
              result == DSM_OK ? data.data() : nullptr,
              result == DSM_OK ? data.size() : 0);
}

}  // namespace dsm
