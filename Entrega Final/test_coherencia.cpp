// Prueba de coherencia MSI entre dos procesos (nodos 0 y 1 de test_coherencia.conf).
// Uso: ./test_coherencia 0 & sleep 1; ./test_coherencia 1
// El nodo 0 arranca primero y asigna en su propio rango, así la región empieza en
// dsm_base y el nodo 1 la encuentra sin consultar la tabla de páginas.
#include "dsm.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>

namespace {

volatile char* const shared = reinterpret_cast<volatile char*>(0x100000000000ULL);

void waitFor(int index, char value) {
    while (shared[index] != value) {
        usleep(1000);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    assert(argc == 2);
    const int node = argv[1][0] - '0';
    assert(dsm_init("test_coherencia.conf", node) == DSM_OK);
    const size_t page = dsm_page_size();

    if (node == 0) {
        void* data = nullptr;
        assert(dsm_alloc(2 * page, 7, &data) == DSM_OK);
        assert(data == const_cast<char*>(shared));
        shared[100] = 42;          // M en el home.
        shared[page + 5] = 9;      // Segunda página.
        shared[0] = 1;             // Datos listos.

        waitFor(1, 1);             // Lee la página que el nodo 1 tiene en M (write-back).
        assert(shared[200] == 7);
        assert(shared[page + 6] == 8);
        shared[2] = 1;             // Escritura: invalida la copia del nodo 1.

        dsm_stats_t stats{};
        assert(dsm_stats(0, &stats) == DSM_OK);
        assert(stats.pages_sent >= 2 && stats.pages_recv >= 2);
        waitFor(3, 1);             // El nodo 1 vio el último valor.
        assert(dsm_free(data) == DSM_OK);
        std::printf("test_coherencia nodo 0: OK (enviadas %u, recibidas %u)\n",
                    stats.pages_sent, stats.pages_recv);
    } else {
        waitFor(0, 1);             // Lectura remota: el home baja de M a S.
        assert(shared[100] == 42);
        assert(shared[page + 5] == 9);
        shared[200] = 7;           // Escritura remota: el nodo 1 queda en M.
        shared[page + 6] = 8;
        shared[1] = 1;

        waitFor(2, 1);             // Su copia fue invalidada y vuelve a pedirla.
        shared[3] = 1;
        dsm_stats_t stats{};
        assert(dsm_stats(1, &stats) == DSM_OK);
        assert(stats.faults >= 4 && stats.pages_recv >= 3);
        std::printf("test_coherencia nodo 1: OK (fallos %u)\n", stats.faults);
        sleep(1);                  // Deja que el nodo 0 libere mientras sigue en línea.
    }
    dsm_shutdown();
    return 0;
}
