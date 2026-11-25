// Programa dsmnode: consola del nodo (nodes, stats, pages, mensajes).
#include "dsm_internal.h"

namespace dsm {

void printNodes(const Node& node, std::mutex& output_mutex) {
    const auto entries = node.nodes();
    const Config& config = node.config();
    std::lock_guard<std::mutex> lock(output_mutex);
    // Encabezados y valores con el mismo formato; solo ASCII para que printf
    // cuente bien los anchos.
    const char* format = "%-3s %-9s %-20s %6s %8s %10s\n";
    std::printf("\nNodos configurados (%zu):\n", entries.size());
    std::printf(format, "ID", "Estado", "Host", "Datos", "Control", "Heartbeat");
    for (const auto& entry : entries) {
        std::printf(format, std::to_string(entry.node_id).c_str(),
                    entry.state == static_cast<std::uint8_t>(NodeState::Online)
                        ? "EN LINEA"
                        : "CAIDO",
                    config.hosts[entry.node_id],
                    std::to_string(channelPort(config, entry.node_id, Channel::Data)).c_str(),
                    std::to_string(channelPort(config, entry.node_id, Channel::Control)).c_str(),
                    std::to_string(channelPort(config, entry.node_id, Channel::Heartbeat)).c_str());
    }
    std::fflush(stdout);
}

void printHelp() {
    std::lock_guard<std::mutex> lock(console_output_mutex);
    std::cout << "Comandos:\n"
                 "  nodes                             nodos configurados y su estado\n"
                 "  stats                             estadísticas de todos los nodos\n"
                 "  pages <node_id>                   páginas asignadas en ese home\n"
                 "  run <wordcount|matmul> [args]     corre una aplicación en este nodo\n"
                 "  monitor                           mensajes recibidos y cambios de nodos (Enter sale)\n"
                 "  send <id> <data|control> <texto>  mensaje de prueba a un nodo\n"
                 "  broadcast <data|control> <texto>  mensaje de prueba a todos\n"
                 "  help, quit\n"
              << std::flush;
}

std::uint8_t parseNodeId(const std::string& value) {
    const std::uint64_t node_id = parseUnsigned(value, "node_id");
    if (node_id >= MAX_NODES) {
        throw std::runtime_error("node_id debe estar entre 0 y 31");
    }
    return static_cast<std::uint8_t>(node_id);
}

const char* stateName(std::uint8_t state) {
    return state == static_cast<std::uint8_t>(PageState::Modified) ? "M"
         : state == static_cast<std::uint8_t>(PageState::Shared)   ? "S"
                                                                   : "I";
}

// stats: estadísticas de todos los nodos y del sistema.
// pages <id>: páginas asignadas cuyo home es el nodo id (directorio MSI).
bool handleQueryCommand(Node& node, const std::string& command) {
    std::istringstream input(command);
    std::string operation;
    input >> operation;
    if (operation != "stats" && operation != "pages") {
        return false;
    }
    const auto entries = node.nodes();
    std::lock_guard<std::mutex> lock(console_output_mutex);

    if (operation == "stats") {
        NodeStats total{};
        std::printf("\nID  Total  Libres    M    S      I  Fallos  Enviadas  Recibidas\n");
        for (const auto& entry : entries) {
            NodeStats stats{};
            const int result = entry.state == static_cast<std::uint8_t>(NodeState::Online)
                ? node.queryStats(entry.node_id, stats)
                : DSM_E_NODE_DOWN;
            if (result != DSM_OK) {
                std::printf("%-3u sin respuesta (%s)\n",
                            static_cast<unsigned int>(entry.node_id), dsm_strerror(result));
                continue;
            }
            std::printf("%-3u %5u  %6u %4u %4u %6u  %6u  %8u  %9u\n",
                        static_cast<unsigned int>(entry.node_id), stats.total_pages,
                        stats.free_pages, stats.pages_m, stats.pages_s, stats.pages_i,
                        stats.faults, stats.pages_sent, stats.pages_recv);
            total.total_pages += stats.total_pages;
            total.free_pages += stats.free_pages;
            total.faults += stats.faults;
            total.pages_sent += stats.pages_sent;
            total.pages_recv += stats.pages_recv;
        }
        std::printf("Sistema: %u páginas, %u libres (%u%% en uso), %u fallos, "
                    "%u páginas enviadas, %u recibidas.\n"
                    "M/S/I cuentan las copias que cada nodo tiene de todo el espacio DSM.\n",
                    total.total_pages, total.free_pages,
                    total.total_pages == 0 ? 0U
                        : 100U * (total.total_pages - total.free_pages) / total.total_pages,
                    total.faults, total.pages_sent, total.pages_recv);
        std::fflush(stdout);
        return true;
    }

    std::string target_text;
    std::uint8_t target = 0;
    try {
        if (!(input >> target_text)) {
            throw std::runtime_error("falta node_id");
        }
        target = parseNodeId(target_text);
        if (target >= entries.size()) {
            throw std::runtime_error("node_id no configurado");
        }
    } catch (const std::exception& error) {
        std::printf("Uso: pages <node_id> (%s)\n", error.what());
        return true;
    }
    const NodeInfo& info = entries[target];
    const Config& config = node.config();
    std::printf("\nPáginas asignadas con home en el nodo %u (estado = copia del home):\n"
                "Página  Dirección       Proceso  Orden  Dueño  Estado  Copias\n",
                static_cast<unsigned int>(target));
    unsigned int shown = 0;
    for (std::uint32_t first = info.first_page; first < info.first_page + info.num_pages;
         first += QUERY_MAX) {
        const auto count = std::min<std::uint32_t>(
            QUERY_MAX, info.first_page + info.num_pages - first);
        std::vector<PageEntry> pages;
        const int result = node.queryPageTable(target, first, count, pages);
        if (result != DSM_OK) {
            std::printf("Sin respuesta del nodo %u: %s\n",
                        static_cast<unsigned int>(target), dsm_strerror(result));
            return true;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            const PageEntry& entry = pages[index];
            if (entry.proc_id == 0) {
                continue;
            }
            std::string copies;
            for (std::size_t id = 0; id < entries.size(); ++id) {
                if ((entry.copyset & nodeBit(static_cast<std::uint8_t>(id))) != 0) {
                    copies += (copies.empty() ? "" : ",") + std::to_string(id);
                }
            }
            std::printf("%6u  0x%012llx  %7u  %5u  %5u  %6s  %s\n", first + index,
                        static_cast<unsigned long long>(
                            config.dsm_base +
                            static_cast<std::uint64_t>(first + index) * config.page_size),
                        entry.proc_id, entry.order, static_cast<unsigned int>(entry.owner),
                        stateName(entry.state), copies.c_str());
            ++shown;
        }
    }
    std::printf("%u de %u páginas asignadas.\n", shown, info.num_pages);
    std::fflush(stdout);
    return true;
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


}  // namespace dsm

extern "C" int wordcount_run(int node_id, int argc, char* argv[]);
extern "C" int matmul_run(int node_id, int argc, char* argv[]);

// run <app> [argumentos]: corre la aplicación en un hilo de este nodo, sin
// detenerlo; la consola sigue disponible mientras tanto.
void runApp(dsm::Node& node, const std::string& command) {
    std::istringstream input(command);
    std::string word;
    std::string app;
    input >> word >> app;
    int (*entry)(int, int, char**) = app == "wordcount" ? wordcount_run
                                   : app == "matmul"    ? matmul_run
                                                        : nullptr;
    if (entry == nullptr) {
        std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
        std::cout << "Uso: run <wordcount|matmul> [argumentos]\n" << std::flush;
        return;
    }
    std::vector<std::string> args;
    for (std::string arg; input >> arg;) {
        args.push_back(arg);
    }
    const int node_id = node.nodeId();
    node.launchUserThread([entry, app, args, node_id]() mutable {
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        const int result = entry(node_id, static_cast<int>(argv.size()), argv.data());
        std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
        std::cout << app << " terminó (código " << result << ")\n" << std::flush;
    });
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " <ruta-dsm.conf> <node_id>\n";
        return 2;
    }

    try {
        const std::uint8_t node_id = dsm::parseNodeId(argv[2]);
        // El nodo se crea con el API para que las aplicaciones lanzadas con run
        // usen este mismo nodo.
        const int init_result = dsm_init(argv[1], node_id);
        if (init_result != DSM_OK) {
            std::cerr << "Error: " << dsm_strerror(init_result) << '\n';
            return 1;
        }
        dsm::Node& node = *dsm::apiNode();

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
                    for (std::size_t id = 0; id < current.size(); ++id) {
                        if (id >= previous.size() || current[id].state != previous[id].state) {
                            dsm::monitorEvent(
                                "Nodo " + std::to_string(id) + ": " +
                                (current[id].state ==
                                         static_cast<std::uint8_t>(dsm::NodeState::Online)
                                     ? "EN LINEA"
                                     : "CAIDO"));
                        }
                    }
                    previous = current;
                }
            }
        });

        std::cout << "Nodo " << static_cast<unsigned int>(node_id) << " iniciado.\n";
        dsm::printNodes(node, dsm::console_output_mutex);
        dsm::printHelp();
        const auto prompt = [] {
            std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
            std::cout << "dsm> " << std::flush;
            return true;
        };
        std::string command;
        while (prompt() && std::getline(std::cin, command)) {
            command = dsm::trim(command);
            if (command == "quit" || command == "exit") {
                break;
            }
            if (command == "nodes") {
                dsm::printNodes(node, dsm::console_output_mutex);
            } else if (command == "help") {
                dsm::printHelp();
            } else if (command == "monitor") {
                dsm::printNodes(node, dsm::console_output_mutex);
                dsm::setMonitorLive(true);
                std::string ignored;
                std::getline(std::cin, ignored);
                dsm::setMonitorLive(false);
            } else if (command == "run" || command.rfind("run ", 0) == 0) {
                runApp(node, command);
            } else if (dsm::handleQueryCommand(node, command) ||
                       dsm::handleDemoCommand(node, command)) {
                continue;
            } else if (!command.empty()) {
                std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
                std::cout << "Comando desconocido. Escriba help para ver los comandos.\n"
                          << std::flush;
            }
        }

        monitor_running = false;
        monitor.join();
        // Primero terminan las aplicaciones: dsm_shutdown las esperaría con el
        // mutex del API tomado, y ellas lo necesitan para terminar.
        node.waitUserThreads();
        dsm_shutdown();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
