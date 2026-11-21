// API pública en C (dsm.h) sobre el nodo de este proceso.
#include "dsm_internal.h"

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
    if (node_id != api_node->nodeId() &&
        nodes[static_cast<std::size_t>(node_id)].state !=
            static_cast<std::uint8_t>(dsm::NodeState::Online)) {
        return DSM_E_NODE_DOWN;
    }

    dsm::NodeStats source{};
    const int result =
        api_node->queryStats(static_cast<std::uint8_t>(node_id), source);
    if (result != DSM_OK) {
        return result;
    }
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
    if (node_id != api_node->nodeId() &&
        nodes[static_cast<std::size_t>(node_id)].state !=
            static_cast<std::uint8_t>(dsm::NodeState::Online)) {
        return DSM_E_NODE_DOWN;
    }

    std::vector<dsm::PageEntry> pages;
    const int result = api_node->queryPageTable(static_cast<std::uint8_t>(node_id),
                                                first_page, count, pages);
    if (result != DSM_OK) {
        return result;
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

extern "C" void* dsm_page_address(uint32_t page) {
    std::lock_guard<std::mutex> lock(api_mutex);
    if (!api_node) {
        return nullptr;
    }
    const dsm::Config& config = api_node->config();
    if (page >= static_cast<std::uint32_t>(config.num_hosts) * config.pages_per_node) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(
        config.dsm_base + static_cast<std::uint64_t>(page) * config.page_size));
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
