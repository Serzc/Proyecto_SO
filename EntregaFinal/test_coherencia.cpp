// Prueba de coherencia MSI entre dos procesos (nodos 0 y 1 de test_coherencia.conf).
// Uso: ./test_coherencia 0 & sleep 1; ./test_coherencia 1
// El proceso 8 tiene su segmento en el nodo 0 (8 % 2), así la región empieza en
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
        assert(dsm_alloc(2 * page, 8, &data) == DSM_OK);
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

        // Segmento de 20 páginas con 16 por nodo: no cabe en el nodo 0 (ancla, 10 % 2),
        // que toma su final libre (páginas 2-15); las 6 restantes salen del nodo 1.
        char* big = nullptr;
        assert(dsm_alloc(20 * page, 10, reinterpret_cast<void**>(&big)) == DSM_OK);
        assert(big == const_cast<char*>(shared) + 2 * page);
        waitFor(4, 1);             // El nodo 1 escribió a ambos lados del límite.
        assert(big[13 * page] == 'a' && big[15 * page] == 'b' && big[19 * page] == 'c');
        dsm_page_t piece[6]{};
        assert(dsm_page_table(1, 16, 6, piece) == 6);
        assert(piece[0].proc_id == 10 && piece[0].order == 14 && piece[5].order == 19);
        shared[5] = 1;
        waitFor(6, 1);             // El nodo 1 ya no lee el segmento del proceso 8.
        assert(dsm_free(big) == DSM_OK);
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

        // Consultas remotas (QUERY_REQUEST) al home.
        dsm_stats_t home{};
        assert(dsm_stats(0, &home) == DSM_OK);
        assert(home.total_pages == 16 && home.free_pages == 14);
        dsm_page_t pages[2]{};
        assert(dsm_page_table(0, 0, 2, pages) == 2);
        assert(pages[0].proc_id == 8 && pages[1].proc_id == 8 && pages[1].order == 1);
        assert(pages[0].owner == 0);
        assert(dsm_page_table(0, 15, 2, pages) == DSM_E_INVAL);  // Fuera del rango del nodo 0.
        shared[3] = 1;

        // Se une al segmento de 20 páginas que ocupa los dos nodos.
        char* big = nullptr;
        assert(dsm_alloc(20 * page, 10, reinterpret_cast<void**>(&big)) == DSM_OK);
        assert(big == const_cast<char*>(shared) + 2 * page);
        big[13 * page] = 'a';      // Página 15: home nodo 0.
        big[15 * page] = 'b';      // Página 17: home nodo 1.
        big[19 * page] = 'c';
        shared[4] = 1;
        waitFor(5, 1);
        shared[6] = 1;
        assert(dsm_free(big) == DSM_OK);
        dsm_stats_t stats{};
        assert(dsm_stats(1, &stats) == DSM_OK);
        assert(stats.faults >= 4 && stats.pages_recv >= 3);
        std::printf("test_coherencia nodo 1: OK (fallos %u)\n", stats.faults);
        sleep(1);                  // Deja que el nodo 0 libere mientras sigue en línea.
    }
    dsm_shutdown();
    return 0;
}
