// Prueba del día 2: mmap, mprotect y fallos de página en un nodo local.
// Compilar: g++ -std=c++17 -pthread -DDSM_NO_MAIN test_memoria.cpp prototipo_2.cpp -o test_memoria
#include "dsm.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static dsm_stats_t stats() {
    dsm_stats_t s{};
    assert(dsm_stats(0, &s) == DSM_OK);
    return s;
}

int main() {
    assert(dsm_init("test_memoria.conf", 0) == DSM_OK);
    const size_t page = dsm_page_size();

    char* data = nullptr;
    assert(dsm_alloc(2 * page, 1, reinterpret_cast<void**>(&data)) == DSM_OK);

    // Página 0: lectura (I -> S), luego escritura (S -> M).
    assert(data[0] == 0);
    assert(stats().faults == 1 && stats().pages_s == 1);
    data[0] = 'A';
    assert(stats().faults == 2 && stats().pages_m == 1 && stats().pages_s == 0);

    // Página 1: escritura directa, dos fallos (I -> S y el reintento S -> M).
    std::strcpy(data + page, "hola DSM");
    assert(stats().faults == 4 && stats().pages_m == 2);

    // Accesos a páginas ya en M no generan fallos.
    data[1] = 'B';
    assert(stats().faults == 4);
    assert(std::strcmp(data + page, "hola DSM") == 0);

    // Al liberar, las páginas vuelven a I y se limpian para el siguiente proceso.
    assert(dsm_free(data) == DSM_OK);
    assert(stats().pages_m == 0 && stats().pages_i == 16);
    char* again = nullptr;
    assert(dsm_alloc(page, 2, reinterpret_cast<void**>(&again)) == DSM_OK);
    assert(again == data && again[0] == 0);
    assert(dsm_free(again) == DSM_OK);

    dsm_shutdown();
    // La región se puede volver a crear tras el shutdown.
    assert(dsm_init("test_memoria.conf", 0) == DSM_OK);
    dsm_shutdown();
    std::puts("test_memoria: OK");
    return 0;
}
