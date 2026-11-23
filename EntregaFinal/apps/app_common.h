/* Utilidades compartidas por las aplicaciones de prueba. Solo usan el API de dsm.h. */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#define _DEFAULT_SOURCE /* usleep con -std=c11 */

#include "../dsm.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define APP_MAX_NODES 32

/* Termina con un mensaje si una llamada al DSM falla. */
static void app_check(int result, const char* what) {
    if (result < 0) {
        fprintf(stderr, "%s: %s\n", what, dsm_strerror(result));
        exit(1);
    }
}

/* Cantidad de nodos configurados. */
static int app_node_count(void) {
    dsm_node_t nodes[APP_MAX_NODES];
    const int count = dsm_nodes(nodes, APP_MAX_NODES);
    app_check(count, "dsm_nodes");
    return count;
}

/* Espera a que todos los nodos configurados estén en línea. */
static void app_wait_all_online(void) {
    for (;;) {
        dsm_node_t nodes[APP_MAX_NODES];
        const int count = dsm_nodes(nodes, APP_MAX_NODES);
        app_check(count, "dsm_nodes");
        int online = 0;
        for (int i = 0; i < count; ++i) {
            online += nodes[i].state == 1;
        }
        if (online == count) {
            return;
        }
        printf("Esperando nodos: %d de %d en línea\n", online, count);
        fflush(stdout);
        sleep(1);
    }
}

/* Busca en las tablas de páginas de todos los nodos la asignación del proceso
   proc (la página con orden 0) y devuelve su dirección, o NULL si no existe. */
static void* app_find(uint16_t proc) {
    dsm_node_t nodes[APP_MAX_NODES];
    const int count = dsm_nodes(nodes, APP_MAX_NODES);
    app_check(count, "dsm_nodes");
    dsm_page_t pages[256];
    for (int n = 0; n < count; ++n) {
        if (nodes[n].state != 1) {
            continue;
        }
        for (uint32_t first = 0; first < nodes[n].num_pages; first += 256) {
            uint32_t chunk = nodes[n].num_pages - first;
            chunk = chunk > 256 ? 256 : chunk;
            if (dsm_page_table(n, nodes[n].first_page + first, chunk, pages) < 0) {
                break;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                if (pages[i].proc_id == proc && pages[i].order == 0) {
                    return dsm_page_address(nodes[n].first_page + first + i);
                }
            }
        }
    }
    return NULL;
}

/* Espera hasta que otro nodo publique la asignación del proceso proc. */
static void* app_wait_find(uint16_t proc) {
    void* address;
    while ((address = app_find(proc)) == NULL) {
        usleep(200 * 1000);
    }
    return address;
}

/* Espera a que una variable compartida tome un valor. La lectura usa la copia S
   local; cuando otro nodo escribe, la invalidación obliga a pedir la página nueva. */
static void app_wait_value(volatile uint32_t* value, uint32_t expected) {
    while (*value != expected) {
        usleep(1000);
    }
}

static void app_print_stats(int node_id) {
    dsm_stats_t stats;
    if (dsm_stats(node_id, &stats) == DSM_OK) {
        printf("Nodo %d: %u fallos de página, %u páginas enviadas, %u recibidas\n",
               node_id, stats.faults, stats.pages_sent, stats.pages_recv);
    }
}

#endif
