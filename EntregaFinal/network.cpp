// Red: listeners TCP, validación y despacho de mensajes, solicitudes
// pendientes, heartbeat, JOIN, hilo de servicio y consultas.
#include "dsm_internal.h"

namespace dsm {

bool Node::sendDemoMessage(std::optional<std::uint8_t> target, Channel channel,
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

int Node::remoteQuery(std::uint8_t target, std::uint32_t kind, std::uint32_t first_page,
                std::uint32_t count, std::vector<std::uint8_t>& reply) {
    std::vector<std::uint8_t> request;
    appendU32(request, kind);
    appendU32(request, first_page);
    appendU32(request, count);
    return requestMessage(target, MessageType::QueryRequest, PAGE_NONE, request,
                          MessageType::QueryReply, reply);
}

// Hilo de red: solo copia datos locales, no espera a otros nodos.
void Node::handleQuery(const MsgHeader& request, const std::vector<std::uint8_t>& payload) {
    std::uint32_t kind = 0;
    std::uint32_t first_page = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> data;
    bool valid = readU32(payload, 0, kind) && readU32(payload, 4, first_page) &&
                 readU32(payload, 8, count);
    if (valid && kind == QUERY_STATS) {
        const NodeStats own = stats()[node_id_];
        for (const std::uint32_t value :
             {own.total_pages, own.free_pages, own.pages_m, own.pages_s,
              own.pages_i, own.faults, own.pages_sent, own.pages_recv}) {
            appendU32(data, value);
        }
    } else if (valid && kind == QUERY_PAGES && count > 0 && count <= QUERY_MAX) {
        std::vector<PageEntry> pages(count);
        valid = copyPageTable(node_id_, first_page, count, pages.data());
        for (const PageEntry& entry : pages) {
            appendU32(data, entry.proc_id);
            appendU32(data, entry.copyset);
            appendU32(data, entry.order);
            data.push_back(entry.state);
            data.push_back(entry.owner);
            data.push_back(0);
            data.push_back(0);
        }
    } else {
        valid = false;
    }
    MsgHeader reply{};
    reply.type = static_cast<std::uint8_t>(MessageType::QueryReply);
    reply.status = valid ? STATUS_OK : STATUS_INVALID;
    reply.seq = request.seq;
    reply.page = PAGE_NONE;
    sendFrame(request.sender, Channel::Control, reply,
              valid ? data.data() : nullptr, valid ? data.size() : 0);
}

// Las solicitudes que esperan a otros nodos se atienden aquí y no en el hilo
// de red, porque las respuestas que esperan llegan por el hilo de red.
void Node::serviceLoop() {
    while (true) {
        ServiceJob job;
        {
            std::unique_lock<std::mutex> lock(service_mutex_);
            service_ready_.wait(lock, [&] { return !running_ || !service_queue_.empty(); });
            if (service_queue_.empty()) {
                return;
            }
            job = std::move(service_queue_.front());
            service_queue_.pop_front();
        }
        try {
            if (job.header.type == static_cast<std::uint8_t>(MessageType::PageRequest)) {
                handlePageRequest(job.header, job.payload);
            } else {
                handleFreeRequest(job.header, job.payload);
            }
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(console_output_mutex);
            std::cerr << "dsm: " << error.what() << '\n';
        }
    }
}

int Node::openListener(Channel channel) const {
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

bool Node::setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void Node::networkLoop() {
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

void Node::acceptConnections(Channel channel, std::vector<Connection>& connections) {
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

bool Node::readMessage(Connection& connection, short events) {
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

bool Node::validMessage(const Connection& connection) const {
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
    case MessageType::QueryRequest:
        return header.seq != 0 && header.status == STATUS_OK &&
               header.page == PAGE_NONE &&
               header.payload_len == QUERY_REQUEST_SIZE;
    case MessageType::QueryReply:
        return header.seq != 0 && header.page == PAGE_NONE &&
               ((header.status == STATUS_OK &&
                 (header.payload_len == QUERY_STATS_SIZE ||
                  (header.payload_len % QUERY_PAGE_ENTRY_SIZE == 0 &&
                   header.payload_len <= QUERY_MAX * QUERY_PAGE_ENTRY_SIZE))) ||
                (header.status == STATUS_INVALID && header.payload_len == 0));
    case MessageType::PageRequest:
        return header.seq != 0 && header.status == STATUS_OK &&
               header.page < page_table_.size() &&
               homeOf(header.page) == node_id_ &&
               header.payload_len == PAGE_INDEX_SIZE;
    case MessageType::PageData:
        return header.seq != 0 && header.page < page_table_.size() &&
               homeOf(header.page) == header.sender &&
               ((header.status == STATUS_OK &&
                 header.payload_len == config_.page_size) ||
                (header.status == STATUS_INVALID && header.payload_len == 0));
    case MessageType::Invalidate:
        return header.seq != 0 && header.status == STATUS_OK &&
               header.page < page_table_.size() &&
               homeOf(header.page) == header.sender &&
               header.payload_len == 0;
    case MessageType::InvalidateAck:
        return header.seq != 0 && header.status == STATUS_OK &&
               header.page < page_table_.size() &&
               homeOf(header.page) == node_id_ &&
               (header.payload_len == 0 ||
                header.payload_len == config_.page_size);
    default:
        return false;
    }
}

void Node::handleMessage(Connection& connection) {
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
            header.flags == DEMO_BROADCAST ? "broadcast" : "directo";
        monitorEvent(std::string("Mensaje ") + delivery + " del nodo " +
                     std::to_string(header.sender) + " por " + channel_name + ": " +
                     message);
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
    case MessageType::PageData:
    case MessageType::InvalidateAck:
    case MessageType::QueryReply:
        completePending(header, std::move(connection.payload));
        break;
    case MessageType::QueryRequest:
        handleQuery(header, connection.payload);
        break;
    case MessageType::AllocRequest:
        handleAllocRequest(header, connection.payload);
        break;
    case MessageType::Invalidate:
        handleInvalidate(header);
        break;
    case MessageType::Free:
    case MessageType::PageRequest: {
        std::lock_guard<std::mutex> lock(service_mutex_);
        service_queue_.push_back({header, std::move(connection.payload)});
        service_ready_.notify_one();
        break;
    }
    default:
        break;
    }
}

void Node::handleAllocRequest(const MsgHeader& request,
                        const std::vector<std::uint8_t>& payload) {
    std::uint32_t count = 0;
    std::uint32_t proc_id = 0;
    std::uint32_t first_page = 0;
    std::uint8_t status = STATUS_INVALID;
    if (readU32(payload, 0, count) && readU32(payload, 4, proc_id)) {
        const int result = allocateAtHome(count, proc_id, request.sender, first_page);
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
        releaseAtHome(first_page, count, proc_id, request.sender);
    }
}

void Node::handleFreeRequest(const MsgHeader& request,
                       const std::vector<std::uint8_t>& payload) {
    std::uint32_t first_page = 0;
    std::uint32_t count = 0;
    std::uint32_t proc_id = 0;
    std::uint8_t status = STATUS_INVALID;
    if (readU32(payload, 0, first_page) &&
        readU32(payload, 4, count) &&
        readU32(payload, 8, proc_id) &&
        releaseAtHome(first_page, count, proc_id, request.sender) == DSM_OK) {
        status = STATUS_OK;
    }

    MsgHeader reply{};
    reply.type = static_cast<std::uint8_t>(MessageType::FreeReply);
    reply.status = status;
    reply.seq = request.seq;
    reply.page = PAGE_NONE;
    sendFrame(request.sender, Channel::Control, reply, nullptr, 0);
}

void Node::completePending(const MsgHeader& response,
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
            : response.type == static_cast<std::uint8_t>(MessageType::FreeReply) ||
                      response.type == static_cast<std::uint8_t>(MessageType::PageData)
                ? DSM_E_BADADDR
            : response.type == static_cast<std::uint8_t>(MessageType::QueryReply)
                ? DSM_E_INVAL
                : DSM_E_NET;
    pending->complete = true;
    pending->ready.notify_one();
}

void Node::failPendingRequests() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& entry : pending_) {
        entry.second->result = DSM_E_NET;
        entry.second->complete = true;
        entry.second->ready.notify_one();
    }
}

int Node::requestMessage(std::uint8_t target, MessageType type, std::uint32_t page,
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

int Node::sendFreeRequest(std::uint8_t home, std::uint32_t first_page,
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

void Node::joinLoop() {
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

bool Node::sendFrame(std::uint8_t target_id, Channel channel, MsgHeader header,
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

bool Node::sendHeartbeat(std::uint8_t target_id) const {
    MsgHeader header{};
    header.type = HEARTBEAT_MESSAGE;
    header.page = PAGE_NONE;
    return sendFrame(target_id, Channel::Heartbeat, header, nullptr, 0);
}

void Node::heartbeatLoop() {
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

}  // namespace dsm
