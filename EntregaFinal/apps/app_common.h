/* Utilidades compartidas por las aplicaciones de prueba. Solo usan el API de dsm.h. */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#define _DEFAULT_SOURCE /* usleep con -std=c11 */

#include "../dsm.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define APP_MAX_NODES 32

/* Termina con un mensaje si una llamada al DSM falla.
   shortcut: dentro de dsmnode (run) esto cierra también la consola; devolver el error
   si se necesita que el nodo sobreviva a una aplicación fallida. */
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

/* Une este nodo al segmento compartido del proceso proc. Todos los nodos piden el
   mismo tamaño y reciben la misma dirección; nadie lo crea ni es su dueño. Las
   primeras `nodes` páginas son de control, una por nodo (fase y resultados).
   Devuelve NULL si el segmento quedó de una corrida interrumpida. */
static char* app_join(uint16_t proc, size_t bytes, int node_id) {
    char* segment = NULL;
    app_check(dsm_alloc(bytes, proc, (void**)&segment), "dsm_alloc");
    if (*(volatile uint32_t*)(segment + node_id * dsm_page_size()) != 0) {
        fprintf(stderr, "El segmento del proceso %u quedó de una corrida anterior "
                        "interrumpida; reinicie los nodos.\n", proc);
        dsm_free(segment);
        return NULL;
    }
    return segment;
}

/* Barrera sin coordinador: el nodo anota que llegó a `phase` en su página de
   control y espera a que todos lleguen. La espera lee copias S locales; cuando
   otro nodo escribe su fase, la invalidación obliga a pedir la página nueva. */
static void app_barrier(char* segment, int node_id, int nodes, uint32_t phase) {
    const size_t page = dsm_page_size();
    *(volatile uint32_t*)(segment + node_id * page) = phase;
    for (int n = 0; n < nodes; ++n) {
        while (*(volatile uint32_t*)(segment + n * page) < phase) {
            usleep(1000);
        }
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
