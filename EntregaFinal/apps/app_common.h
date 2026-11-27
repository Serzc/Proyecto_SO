/* Utilidades compartidas por las aplicaciones de prueba. Solo usan el API de dsm.h.
 *
 * Modelo de ejecución: el nodo donde se escribe `run <app> <archivos>` es el origen
 * de esa corrida (cualquier nodo puede serlo). Es el único que lee los archivos:
 * copia la entrada al DSM y publica sus parámetros en el segmento de información.
 * Los demás nodos reciben `run <app>` sin archivos, leen los parámetros, se unen a
 * los segmentos de datos y leen la entrada a través del DSM. Todos calculan su parte;
 * el origen verifica el resultado, lo guarda en output/ y publica el veredicto.
 */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#define _DEFAULT_SOURCE /* usleep con -std=c11 */

#include "../dsm.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APP_MAX_NODES 32
#define APP_THREADS 4 /* pthreads de trabajo por nodo */

/* Termina con un mensaje si una llamada al DSM falla.
   shortcut: dentro de dsmnode (run) esto cierra también la consola; devolver el error
   si se necesita que el nodo sobreviva a una aplicación fallida. */
static void app_check(int result, const char* what) {
    if (result < 0) {
        fprintf(stderr, "%s: %s\n", what, dsm_strerror(result));
        exit(1);
    }
}

/* Abre output/<name><extension> para escribir el resultado (solo lo hace el nodo
   origen). En path queda la ruta; devuelve NULL, con el error impreso, si falla. */
static FILE* app_output(const char* name, const char* extension, char* path,
                        size_t path_size) {
    if (mkdir("output", 0755) != 0 && errno != EEXIST) {
        perror("output");
        return NULL;
    }
    snprintf(path, path_size, "output/%s%s", name, extension);
    FILE* file = fopen(path, "w");
    if (file == NULL) {
        perror(path);
    }
    return file;
}

/* ---- Segmento de información de una corrida (una página) ---------------------- */

enum { APP_INFO_WAIT = 0, APP_INFO_READY = 1, APP_INFO_CANCELLED = 2 };
enum { APP_VERDICT_NONE = 0, APP_VERDICT_OK = 1, APP_VERDICT_ERROR = 2 };

typedef struct {
    uint32_t state;      /* APP_INFO_*: lo escribe el origen después de los parámetros */
    uint32_t origin;     /* nodo que leyó los archivos */
    uint32_t verdict;    /* APP_VERDICT_*: resultado de la verificación del origen */
    uint32_t reserved;
    uint64_t params[4];  /* tamaños de la entrada, según la app */
    uint8_t joined[APP_MAX_NODES]; /* un byte por nodo: ya se unió (sin escrituras cruzadas) */
} app_info_t;

/* Se une al segmento de información de la app (mismo tamaño en todos los nodos). */
/* Su proceso es data_proc + 100 * nodos: da el mismo resto que data_proc, así que vive
   en el mismo home que el ancla de los datos. Ocupa el principio de ese rango y no el
   del nodo siguiente, que queda libre por si los datos no caben en el ancla. */
static volatile app_info_t* app_info_join(uint16_t data_proc, int nodes) {
    void* info = NULL;
    app_check(dsm_alloc(dsm_page_size(), (uint16_t)(data_proc + 100 * nodes), &info),
              "dsm_alloc información");
    return info;
}

/* Origen: publica los parámetros. Van en la misma página que el estado, así que quien
   ve APP_INFO_READY ya tiene los parámetros en su copia. */
static void app_info_publish(volatile app_info_t* info, int node_id) {
    info->origin = (uint32_t)node_id;
    info->state = APP_INFO_READY;
}

/* Origen: cancela la corrida (archivo inválido, falta memoria) para que nadie espere.
   Antes de soltar el segmento espera a que todos lo hayan visto; si lo soltara antes,
   un nodo que llega tarde crearía uno nuevo y esperaría para siempre. */
static void app_info_cancel(volatile app_info_t* info, int node_id, int nodes) {
    info->origin = (uint32_t)node_id;
    info->state = APP_INFO_CANCELLED;
    for (int n = 0; n < nodes; ++n) {
        while (n != node_id && info->joined[n] == 0) {
            usleep(1000);
        }
    }
}

/* Los demás nodos: esperan los parámetros. Devuelve 1 si la corrida sigue. */
static int app_info_wait(volatile app_info_t* info, int node_id) {
    info->joined[node_id] = 1;
    for (unsigned waited_ms = 0; info->state == APP_INFO_WAIT; ++waited_ms) {
        if (waited_ms == 2000 || (waited_ms > 2000 && (waited_ms - 2000) % 5000 == 0)) {
            printf("Nodo %d: esperando los datos del nodo que lanzó la app\n", node_id);
            fflush(stdout);
        }
        usleep(1000);
    }
    if (info->state == APP_INFO_CANCELLED) {
        printf("Nodo %d: el nodo %u canceló la corrida\n", node_id, info->origin);
        return 0;
    }
    return 1;
}

static const char* app_verdict_name(uint32_t verdict) {
    return verdict == APP_VERDICT_OK ? "CORRECTA" : verdict == APP_VERDICT_ERROR ? "ERROR"
                                                                              : "sin dato";
}

/* ---- pthreads de trabajo -------------------------------------------------------- */

/* Trabajo sobre un rango [begin, end); thread va de 0 a APP_THREADS - 1. */
typedef void (*app_range_fn)(void* context, uint64_t begin, uint64_t end, int thread);

typedef struct {
    app_range_fn fn;
    void* context;
    uint64_t begin;
    uint64_t end;
    int thread;
} app_task_t;

static void* app_task_main(void* argument) {
    app_task_t* task = argument;
    task->fn(task->context, task->begin, task->end, task->thread);
    return NULL;
}

/* Reparte [begin, end) entre APP_THREADS pthreads de este nodo. Los hilos leen y
   escriben el DSM a la vez, así que sus fallos de página llegan concurrentes al
   hilo de fallos del nodo. Si un pthread no se puede crear, ese tramo corre aquí. */
static void app_parallel(app_range_fn fn, void* context, uint64_t begin, uint64_t end) {
    pthread_t threads[APP_THREADS];
    app_task_t tasks[APP_THREADS];
    int created[APP_THREADS];
    for (int i = 0; i < APP_THREADS; ++i) {
        tasks[i].fn = fn;
        tasks[i].context = context;
        tasks[i].begin = begin + (end - begin) * i / APP_THREADS;
        tasks[i].end = begin + (end - begin) * (i + 1) / APP_THREADS;
        tasks[i].thread = i;
        created[i] = pthread_create(&threads[i], NULL, app_task_main, &tasks[i]) == 0;
        if (!created[i]) {
            app_task_main(&tasks[i]);
        }
    }
    for (int i = 0; i < APP_THREADS; ++i) {
        if (created[i]) {
            pthread_join(threads[i], NULL);
        }
    }
}

/* ---- Nodos, segmentos y barreras ------------------------------------------------ */

/* Cantidad de nodos configurados. */
static int app_node_count(void) {
    dsm_node_t nodes[APP_MAX_NODES];
    const int count = dsm_nodes(nodes, APP_MAX_NODES);
    app_check(count, "dsm_nodes");
    return count;
}

/* Memoria de todo el sistema en bytes. */
static inline size_t app_system_bytes(void) {
    dsm_node_t nodes[APP_MAX_NODES];
    const int count = dsm_nodes(nodes, APP_MAX_NODES);
    app_check(count, "dsm_nodes");
    return (size_t)count * nodes[0].num_pages * dsm_page_size();
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
   mismo tamaño y reciben la misma dirección. Las primeras `nodes` páginas son de
   control, una por nodo (fase y resultados). Devuelve NULL si el segmento quedó de
   una corrida interrumpida. */
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

/* Barrera: el nodo anota que llegó a `phase` en su página de control y espera a que
   todos lleguen. La espera lee copias S locales; cuando otro nodo escribe su fase, la
   invalidación obliga a pedir la página nueva. */
static void app_barrier(char* segment, int node_id, int nodes, uint32_t phase) {
    const size_t page = dsm_page_size();
    *(volatile uint32_t*)(segment + node_id * page) = phase;
    for (unsigned waited_ms = 0;; ++waited_ms) {
        int missing = 0;
        for (int n = 0; n < nodes; ++n) {
            missing += *(volatile uint32_t*)(segment + n * page) < phase;
        }
        if (missing == 0) {
            return;
        }
        /* Avisa a los 2 s y luego cada 5 s: casi siempre es un nodo caído o sin dsmnode
           abierto, que no recibió el RUN_REQUEST. */
        if (waited_ms == 2000 || (waited_ms > 2000 && (waited_ms - 2000) % 5000 == 0)) {
            printf("Nodo %d: esperando a los nodos", node_id);
            for (int n = 0; n < nodes; ++n) {
                if (*(volatile uint32_t*)(segment + n * page) < phase) {
                    printf(" %d", n);
                }
            }
            printf(" (¿están en línea con dsmnode abierto?)\n");
            fflush(stdout);
        }
        usleep(1000);
    }
}

/* ---- Mediciones ----------------------------------------------------------------- */

/* Milisegundos de un reloj monotónico, para medir las fases de las apps. */
static double app_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

/* Tiempos de un nodo: carga de la entrada al DSM (hasta la barrera 1), cálculo
   distribuido (de la barrera 1 a la 2) y, solo en el origen, el mismo cálculo completo
   hecho localmente sin DSM (sequential < 0 en los demás). */
static void app_print_times(int node_id, double start, double loaded, double computed,
                            double sequential) {
    printf("Nodo %d: tiempos: carga %.1f ms, cálculo distribuido %.1f ms", node_id,
           loaded - start, computed - loaded);
    if (sequential >= 0) {
        printf(", cálculo secuencial local %.1f ms", sequential);
    }
    printf("\n");
}

static void app_print_stats(int node_id) {
    dsm_stats_t stats;
    if (dsm_stats(node_id, &stats) == DSM_OK) {
        printf("Nodo %d: %u fallos de página, %u páginas enviadas, %u recibidas\n",
               node_id, stats.faults, stats.pages_sent, stats.pages_recv);
    }
}

#endif
