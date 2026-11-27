// Programa dsmnode: consola del nodo (nodes, stats, pages, mensajes).
#include "dsm_internal.h"
#include <termios.h>

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
                 "  pages                             vista de páginas por nodo (flechas, q sale)\n"
                 "  pages <node_id>                   lista las páginas asignadas en ese home\n"
                 "  run <app> <archivos>              corre una aplicación en este nodo:\n"
                 "                                      wordcount <texto>, matmul <a> <b>,\n"
                 "                                      imgfilter <imagen.ppm>\n"
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

// Tabla completa de las páginas cuyo home es info.node_id (local o por QUERY).
int fetchHomePages(Node& node, const NodeInfo& info, std::vector<PageEntry>& out) {
    out.clear();
    for (std::uint32_t first = info.first_page; first < info.first_page + info.num_pages;
         first += QUERY_MAX) {
        const auto count = std::min<std::uint32_t>(
            QUERY_MAX, info.first_page + info.num_pages - first);
        std::vector<PageEntry> chunk;
        const int result = node.queryPageTable(info.node_id, first, count, chunk);
        if (result != DSM_OK) {
            return result;
        }
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return DSM_OK;
}

// "0,2" para un conjunto de nodos en bits.
std::string nodeList(std::uint32_t nodes, std::size_t node_count) {
    std::string text;
    for (std::size_t id = 0; id < node_count; ++id) {
        if ((nodes & nodeBit(static_cast<std::uint8_t>(id))) != 0) {
            text += (text.empty() ? "" : ",") + std::to_string(id);
        }
    }
    return text.empty() ? "-" : text;
}

// Una asignación (páginas contiguas de un proceso) en el home.
struct HomeAllocation {
    std::uint32_t proc_id = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t states[3] = {0, 0, 0};  // Copia del home en I, S y M.
    std::uint32_t copies = 0;             // Nodos con copia de alguna página.
    std::uint32_t owners = 0;             // Nodos con alguna página en M.
};

// Arma la pantalla de la vista de páginas del nodo target (no la imprime).
std::string renderPagesView(Node& node, std::uint8_t target) {
    char* buffer = nullptr;
    std::size_t length = 0;
    FILE* out = open_memstream(&buffer, &length);
    if (out == nullptr) {
        return "No hay memoria para la vista.\n";
    }
    const auto entries = node.nodes();
    const NodeInfo& info = entries[target];
    const Config& config = node.config();
    const bool online = info.state == static_cast<std::uint8_t>(NodeState::Online);

    std::fprintf(out, "Páginas del nodo %u (nodos 0 a %zu)   (<- -> cambia de nodo, q o Enter sale; "
                "se actualiza con cada cambio)\n",
                static_cast<unsigned int>(target), entries.size() - 1);
    std::fprintf(out, "Estado: %s   Host: %s   Rango: páginas %u a %u\n\n",
                online ? "EN LINEA" : "CAIDO", config.hosts[target], info.first_page,
                info.first_page + info.num_pages - 1);
    NodeStats stats{};
    std::vector<PageEntry> pages;
    const int result = online ? node.queryStats(target, stats) : DSM_E_NODE_DOWN;
    const int pages_result = result == DSM_OK ? fetchHomePages(node, info, pages) : result;
    if (pages_result != DSM_OK) {
        std::fprintf(out, "Sin datos: %s\n", dsm_strerror(pages_result));
        std::fclose(out);
        const std::string screen(buffer, length);
        std::free(buffer);
        return screen;
    }

    const std::uint32_t used = stats.total_pages - stats.free_pages;
    std::fprintf(out, "Memoria que aporta: %u páginas de %u bytes; %u usadas, %u libres (%u%% en uso)\n",
                stats.total_pages, config.page_size, used, stats.free_pages,
                stats.total_pages == 0 ? 0U : 100U * used / stats.total_pages);
    std::fprintf(out, "Copias que tiene de todo el DSM: M %u, S %u, I %u\n", stats.pages_m,
                stats.pages_s, stats.pages_i);
    std::fprintf(out, "Fallos atendidos %u, páginas enviadas %u, recibidas %u\n\n", stats.faults,
                stats.pages_sent, stats.pages_recv);

    // Agrupa las páginas en asignaciones y asigna una letra por proceso para el mapa.
    std::vector<HomeAllocation> allocations;
    std::vector<std::uint32_t> procs;
    std::string map(pages.size(), '.');
    for (std::uint32_t index = 0; index < pages.size(); ++index) {
        const PageEntry& entry = pages[index];
        if (entry.proc_id == 0) {
            continue;
        }
        if (entry.order == 0 || allocations.empty() ||
            allocations.back().proc_id != entry.proc_id ||
            allocations.back().first + allocations.back().count != info.first_page + index) {
            allocations.push_back({});
            allocations.back().proc_id = entry.proc_id;
            allocations.back().first = info.first_page + index;
        }
        HomeAllocation& allocation = allocations.back();
        ++allocation.count;
        ++allocation.states[entry.state <= 2 ? entry.state : 0];
        allocation.copies |= entry.copyset;
        if (entry.owner != target && entry.owner < MAX_NODES) {
            allocation.owners |= nodeBit(entry.owner);
        }
        auto letter = std::find(procs.begin(), procs.end(), entry.proc_id);
        if (letter == procs.end()) {
            procs.push_back(entry.proc_id);
            letter = procs.end() - 1;
        }
        const auto position = static_cast<std::size_t>(letter - procs.begin());
        map[index] = position < 26 ? static_cast<char>('A' + position) : '#';
    }

    std::fprintf(out, "Mapa (cada carácter es una página: . libre, letra = proceso):\n");
    for (std::size_t row = 0; row < map.size(); row += 64) {
        std::fprintf(out, "%6zu  %s\n", info.first_page + row, map.substr(row, 64).c_str());
    }

    std::fprintf(out, "\nAsignaciones (%zu):\n", allocations.size());
    if (!allocations.empty()) {
        std::fprintf(out, "  Proc  Letra      Páginas   Cant  Dirección        Home I/S/M  "
                    "Copias en  Escritor\n");
    }
    for (const HomeAllocation& allocation : allocations) {
        const auto letter = std::find(procs.begin(), procs.end(), allocation.proc_id) -
                            procs.begin();
        const std::string range = std::to_string(allocation.first) + "-" +
                                  std::to_string(allocation.first + allocation.count - 1);
        const std::string states = std::to_string(allocation.states[0]) + "/" +
                                   std::to_string(allocation.states[1]) + "/" +
                                   std::to_string(allocation.states[2]);
        std::fprintf(out, "%6u  %5c  %11s  %5u  0x%012llx  %11s  %-9s  %s\n", allocation.proc_id,
                    letter < 26 ? static_cast<char>('A' + letter) : '#', range.c_str(),
                    allocation.count,
                    static_cast<unsigned long long>(
                        config.dsm_base +
                        static_cast<std::uint64_t>(allocation.first) * config.page_size),
                    states.c_str(), nodeList(allocation.copies, entries.size()).c_str(),
                    allocation.owners == 0 ? "home"
                                           : nodeList(allocation.owners, entries.size()).c_str());
    }

    // Log del home: solicitudes recibidas y lo que concedió, más recientes primero.
    std::vector<LogEntry> log;
    const int log_result = node.queryLog(target, log);
    std::fprintf(out, "\nSolicitudes atendidas como home (más recientes primero):\n");
    if (log_result != DSM_OK) {
        std::fprintf(out, "  sin respuesta: %s\n", dsm_strerror(log_result));
    } else if (log.empty()) {
        std::fprintf(out, "  (ninguna)\n");
    }
    const std::size_t shown = std::min<std::size_t>(log.size(), 12);
    for (std::size_t index = 0; index < shown; ++index) {
        const LogEntry& entry = log[log.size() - 1 - index];
        char stamp[16] = "--:--:--";
        const std::time_t when = entry.time;
        std::tm local{};
        if (localtime_r(&when, &local) != nullptr) {
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
        }
        std::fprintf(out, "  %s  nodo %-2u ", stamp, static_cast<unsigned int>(entry.node));
        switch (entry.kind) {
        case LOG_PAGE: {
            const bool write = entry.result == static_cast<std::uint8_t>(PageState::Modified);
            std::fprintf(out, "pág %-5u %s: concedida %s", entry.page,
                         write ? "escritura" : "lectura  ", write ? "M" : "S");
            if (entry.value != 0) {
                std::fprintf(out, "; invalidó %s", nodeList(entry.value, entries.size()).c_str());
            }
            if (entry.extra != NODE_NONE) {
                std::fprintf(out, "; write-back de %u", static_cast<unsigned int>(entry.extra));
            }
            break;
        }
        case LOG_DENIED:
            std::fprintf(out, "pág %-5u %s: rechazada (no asignada)", entry.page,
                         entry.result != 0 ? "escritura" : "lectura  ");
            break;
        case LOG_CREATE:
            std::fprintf(out, "creó el segmento del proceso %u (desde la pág %u)", entry.value,
                         entry.page);
            break;
        case LOG_JOIN:
            std::fprintf(out, "se unió al segmento del proceso %u", entry.value);
            break;
        case LOG_LEAVE:
            std::fprintf(out, "soltó el segmento del proceso %u", entry.value);
            break;
        case LOG_RELEASE:
            std::fprintf(out, "soltó el segmento del proceso %u; páginas liberadas", entry.value);
            break;
        default:
            std::fprintf(out, "evento desconocido %u", static_cast<unsigned int>(entry.kind));
        }
        std::fprintf(out, "\n");
    }
    std::fclose(out);
    const std::string screen(buffer, length);
    std::free(buffer);
    return screen;
}

// Vista interactiva de páginas: flechas izquierda/derecha cambian de nodo.
void pagesView(Node& node) {
    termios saved{};
    const bool raw = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (raw) {
        termios keys = saved;
        keys.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        keys.c_cc[VMIN] = 1;
        keys.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &keys);
    }
    const auto node_count = static_cast<int>(node.nodes().size());
    int target = node.nodeId();
    std::string shown;
    while (true) {
        // Consulta cada 250 ms y redibuja solo si algo cambió, sin parpadeo.
        const std::string screen = renderPagesView(node, static_cast<std::uint8_t>(target));
        if (screen != shown) {
            std::lock_guard<std::mutex> lock(console_output_mutex);
            std::printf("\033[2J\033[H%s", screen.c_str());
            std::fflush(stdout);
            shown = screen;
        }
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 250) == 0) {
            continue;
        }
        const int key = std::cin.get();
        if (key == EOF || key == 'q' || key == '\n') {
            break;
        }
        if (key == 0x1b && std::cin.get() == '[') {  // Flechas: ESC [ C o ESC [ D.
            const int arrow = std::cin.get();
            if (arrow == 'C') {
                target = (target + 1) % node_count;
            } else if (arrow == 'D') {
                target = (target + node_count - 1) % node_count;
            }
        }
    }
    if (raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
    std::cout << '\n';
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
    std::vector<PageEntry> pages;
    const int result = fetchHomePages(node, info, pages);
    if (result != DSM_OK) {
        std::printf("Sin respuesta del nodo %u: %s\n",
                    static_cast<unsigned int>(target), dsm_strerror(result));
        return true;
    }
    unsigned int shown = 0;
    for (std::uint32_t index = 0; index < info.num_pages; ++index) {
        const PageEntry& entry = pages[index];
        if (entry.proc_id == 0) {
            continue;
        }
        const std::uint32_t page = info.first_page + index;
        std::printf("%6u  0x%012llx  %7u  %5u  %5u  %6s  %s\n", page,
                    static_cast<unsigned long long>(
                        config.dsm_base + static_cast<std::uint64_t>(page) * config.page_size),
                    entry.proc_id, entry.order, static_cast<unsigned int>(entry.owner),
                    stateName(entry.state), nodeList(entry.copyset, entries.size()).c_str());
        ++shown;
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
extern "C" int imgfilter_run(int node_id, int argc, char* argv[]);

// run <app> [argumentos]: corre la aplicación en un hilo de este nodo, sin
// detenerlo; la consola sigue disponible mientras tanto.
void runApp(dsm::Node& node, const std::string& command, bool from_console) {
    std::istringstream input(command);
    std::string word;
    std::string app;
    input >> word >> app;
    int (*entry)(int, int, char**) = app == "wordcount" ? wordcount_run
                                   : app == "matmul"    ? matmul_run
                                   : app == "imgfilter" ? imgfilter_run
                                                        : nullptr;
    if (entry == nullptr) {
        std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
        std::cout << "Uso: run <wordcount|matmul|imgfilter> <archivos>\n" << std::flush;
        return;
    }
    std::vector<std::string> args;
    for (std::string arg; input >> arg;) {
        args.push_back(arg);
    }
    if (from_console) {
        if (args.empty()) {
            std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
            std::cout << "Falta la entrada: run " << app
                      << (app == "matmul" ? " <matriz_a> <matriz_b>" : " <archivo>") << '\n'
                      << std::flush;
            return;
        }
        // Este nodo es el origen de la corrida: solo él lee los archivos y los copia al
        // DSM. A los demás les pide la misma app sin archivos (RUN_REQUEST); ellos
        // toman la entrada del DSM.
        std::string sent;
        std::string failed;
        for (const auto& entry_info : node.nodes()) {
            if (entry_info.node_id == node.nodeId()) {
                continue;
            }
            std::string& list =
                node.sendRunRequest(entry_info.node_id, "run " + app) ? sent : failed;
            list += (list.empty() ? "" : ", ") + std::to_string(entry_info.node_id);
        }
        std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
        std::cout << "Ejecutando en este nodo" << (sent.empty() ? "" : " y en los nodos " + sent)
                  << (failed.empty() ? "" : "; sin respuesta de los nodos " + failed +
                                                " (la app los esperará)")
                  << ".\n"
                  << std::flush;
    } else {
        args.clear();  // Un pedido remoto nunca trae archivos: la entrada está en el DSM.
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
        // RUN_REQUEST de otro nodo: corre la misma app aquí, sin reenviarla.
        node.setRunHandler([&node](std::uint8_t sender, const std::string& command) {
            dsm::monitorEvent("El nodo " + std::to_string(sender) + " pidió: " + command);
            {
                std::lock_guard<std::mutex> lock(dsm::console_output_mutex);
                std::cout << "\nEl nodo " << static_cast<unsigned int>(sender)
                          << " pidió ejecutar: " << command << '\n'
                          << std::flush;
            }
            runApp(node, command, false);
        });

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
                    if (dsm::monitorIsLive()) {
                        dsm::printNodes(node, dsm::console_output_mutex);
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
            if (command == "pages") {
                dsm::pagesView(node);
            } else if (command == "nodes") {
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
                runApp(node, command, true);
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
