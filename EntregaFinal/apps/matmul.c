/* Aplicación de prueba 2: multiplicación distribuida de matrices C = A x B.
 *
 * En el nodo origen: run matmul <matriz_a> <matriz_b> (archivos con n y luego n*n
 * números, n <= 128). Los demás nodos la reciben como run matmul, sin archivos.
 *
 * 1. El origen lee A y B, las copia al segmento compartido y publica n.
 * 2. Barrera. Cada nodo calcula su bloque de filas de C con varios pthreads, leyendo
 *    A y B a través del DSM: las páginas le llegan por la red desde su home.
 * 3. Barrera. El origen compara C completa con la multiplicación secuencial, la
 *    guarda en output/matmul_C.txt y publica el veredicto.
 * 4. Barrera y dsm_free; el segmento se libera cuando lo suelta el último nodo.
 */
#include "app_common.h"
#include <string.h>

#define PROC_MATMUL 20

static double* read_matrix(const char* path, uint32_t* n) {
    FILE* file = fopen(path, "r");
    if (file == NULL || fscanf(file, "%u", n) != 1 || *n == 0 || *n > 128) {
        fprintf(stderr, "%s: no existe o el formato es inválido (n entre 1 y 128)\n", path);
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    double* matrix = malloc(sizeof(double) * *n * *n);
    for (uint32_t i = 0; matrix != NULL && i < *n * *n; ++i) {
        if (fscanf(file, "%lf", &matrix[i]) != 1) {
            fprintf(stderr, "%s: faltan valores\n", path);
            free(matrix);
            matrix = NULL;
        }
    }
    fclose(file);
    return matrix;
}

static void multiply_rows(const volatile double* a, const volatile double* b,
                          volatile double* c, uint32_t n, uint32_t first, uint32_t last) {
    for (uint32_t i = first; i < last; ++i) {
        for (uint32_t j = 0; j < n; ++j) {
            double sum = 0;
            for (uint32_t k = 0; k < n; ++k) {
                sum += a[i * n + k] * b[k * n + j];
            }
            c[i * n + j] = sum;
        }
    }
}

typedef struct {
    const double* a;
    const double* b;
    double* c;
    uint32_t n;
} shared_matrices_t;

/* Tarea de un pthread: filas [begin, end) de C sobre las matrices compartidas. */
static void multiply_task(void* context, uint64_t begin, uint64_t end, int thread) {
    (void)thread;
    const shared_matrices_t* m = context;
    multiply_rows(m->a, m->b, m->c, m->n, (uint32_t)begin, (uint32_t)end);
}

/* Corre la aplicación en el nodo ya iniciado de este proceso. Con argumentos este
   nodo es el origen; sin argumentos toma los datos del DSM. */
int matmul_run(int node_id, int argc, char* argv[]) {
    const int origin = argc >= 2;
    if (argc == 1) {
        fprintf(stderr, "Uso: matmul <matriz_a> <matriz_b>\n");
        return 2;
    }
    const int nodes = app_node_count();
    app_wait_all_online();
    volatile app_info_t* info = app_info_join(PROC_MATMUL, nodes);
    double start = app_now_ms();

    double* a = NULL;
    double* b = NULL;
    uint32_t n = 0;
    if (origin) {
        uint32_t n_b = 0;
        a = read_matrix(argv[0], &n);
        b = read_matrix(argv[1], &n_b);
        if (a == NULL || b == NULL || n != n_b) {
            fprintf(stderr, "Las dos matrices deben existir y tener el mismo tamaño.\n");
            app_info_cancel(info, node_id, nodes);
            dsm_free((void*)info);
            free(a);
            free(b);
            return 1;
        }
        info->params[0] = n;
        app_info_publish(info, node_id);
    } else {
        if (!app_info_wait(info, node_id)) {
            dsm_free((void*)info);
            return 1;
        }
        n = (uint32_t)info->params[0];
    }

    start = app_now_ms(); /* La carga se mide desde que la entrada está publicada. */
    const size_t page = dsm_page_size();
    const size_t bytes = sizeof(double) * n * n;
    char* segment = app_join(PROC_MATMUL, nodes * page + 3 * bytes, node_id);
    if (segment == NULL) {
        dsm_free((void*)info);
        free(a);
        free(b);
        return 1;
    }
    double* shared_a = (double*)(segment + nodes * page);
    double* shared_b = shared_a + n * n;
    double* shared_c = shared_b + n * n;
    if (origin) {
        memcpy(shared_a, a, bytes);
        memcpy(shared_b, b, bytes);
        printf("Nodo %d: A y B (%ux%u) cargadas en el DSM (%p).\n", node_id, n, n,
               (void*)segment);
        fflush(stdout);
    }
    app_barrier(segment, node_id, nodes, 1);
    const double loaded = app_now_ms();

    const uint32_t first = n * node_id / nodes;
    const uint32_t last = n * (node_id + 1) / nodes;
    shared_matrices_t shared = {shared_a, shared_b, shared_c, n};
    app_parallel(multiply_task, &shared, first, last);
    printf("Nodo %d: filas [%u, %u) de C calculadas con %d pthreads.\n", node_id, first, last,
           APP_THREADS);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 2);
    const double computed = app_now_ms();

    double sequential = -1;
    if (origin) {
        double* expected = malloc(bytes);
        const double sequential_start = app_now_ms();
        multiply_rows(a, b, expected, n, 0, n);
        sequential = app_now_ms() - sequential_start;
        uint32_t wrong = 0;
        double checksum = 0;
        for (uint32_t i = 0; i < n * n; ++i) {
            wrong += shared_c[i] != expected[i];
            checksum += shared_c[i];
        }
        printf("Nodo %d: C[0][0] = %.4f, C[%u][%u] = %.4f, suma de C = %.4f\n", node_id,
               shared_c[0], n - 1, n - 1, shared_c[n * n - 1], checksum);
        printf("Nodo %d: verificación de C completa contra la multiplicación secuencial: "
               "%s (%u de %u celdas distintas)\n",
               node_id, wrong == 0 ? "CORRECTA" : "ERROR", wrong, n * n);
        /* C en el mismo formato que las matrices de entrada: n y luego n filas. */
        char path[512];
        FILE* output = app_output("matmul_C", ".txt", path, sizeof(path));
        if (output != NULL) {
            fprintf(output, "%u\n", n);
            for (uint32_t i = 0; i < n; ++i) {
                for (uint32_t j = 0; j < n; ++j) {
                    fprintf(output, j + 1 < n ? "%.4f " : "%.4f\n", shared_c[i * n + j]);
                }
            }
            fclose(output);
            printf("Nodo %d: C guardada en %s\n", node_id, path);
        }
        info->verdict = wrong == 0 ? APP_VERDICT_OK : APP_VERDICT_ERROR;
        free(expected);
    }
    app_print_times(node_id, start, loaded, computed, sequential);
    app_print_stats(node_id);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 3);

    const uint32_t verdict = info->verdict;
    if (!origin) {
        printf("Nodo %d: verificación del nodo %u: %s\n", node_id, info->origin,
               app_verdict_name(verdict));
    }
    /* Si el home de un segmento ya terminó, esa memoria ya no existe: no es un error. */
    dsm_free(segment);
    dsm_free((void*)info);
    free(a);
    free(b);
    return verdict == APP_VERDICT_OK ? 0 : 1;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> [matriz_a matriz_b (nodo origen)]\n",
                argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = matmul_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
