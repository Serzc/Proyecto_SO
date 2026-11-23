// Lectura de dsm.conf, resolución de hosts y puertos por nodo.
#include "dsm_internal.h"

namespace dsm {

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

}  // namespace dsm
