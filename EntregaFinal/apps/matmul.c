/* Aplicación de prueba 2: multiplicación distribuida de matrices C = A x B.
 *
 * Todos los nodos son iguales y ejecutan lo mismo: run matmul <matriz_a> <matriz_b>
 * (archivos con n y luego n*n números, n <= 128, iguales en todas las máquinas).
 *
 * 1. Cada nodo carga en el segmento compartido su bloque de filas de A y de B.
 * 2. Barrera. Cada nodo calcula su bloque de filas de C; para eso lee B completa,
 *    así que trae por la red las filas que cargaron los demás.
 * 3. Barrera. Cada nodo lee C completa (filas calculadas por otros) y la compara
 *    con la multiplicación secuencial hecha con sus archivos locales.
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

/* Corre la aplicación en el nodo ya iniciado de este proceso. La usan el comando
   run de dsmnode y el main de abajo. */
int matmul_run(int node_id, int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: matmul <matriz_a> <matriz_b>\n");
        return 2;
    }
    uint32_t n = 0;
    uint32_t n_b = 0;
    double* a = read_matrix(argv[0], &n);
    double* b = read_matrix(argv[1], &n_b);
    if (a == NULL || b == NULL || n != n_b) {
        fprintf(stderr, "Las dos matrices deben existir y tener el mismo tamaño.\n");
        free(a);
        free(b);
        return 1;
    }

    const int nodes = app_node_count();
    app_wait_all_online();
    const size_t page = dsm_page_size();
    const size_t bytes = sizeof(double) * n * n;
    char* segment = app_join(PROC_MATMUL, nodes * page + 3 * bytes, node_id);
    if (segment == NULL) {
        free(a);
        free(b);
        return 1;
    }
    double* shared_a = (double*)(segment + nodes * page);
    double* shared_b = shared_a + n * n;
    double* shared_c = shared_b + n * n;
    const uint32_t first = n * node_id / nodes;
    const uint32_t last = n * (node_id + 1) / nodes;

    memcpy(shared_a + first * n, a + first * n, sizeof(double) * (last - first) * n);
    memcpy(shared_b + first * n, b + first * n, sizeof(double) * (last - first) * n);
    printf("Nodo %d: filas [%u, %u) de A y B cargadas en el DSM (%p).\n", node_id, first,
           last, (void*)segment);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 1);

    multiply_rows(shared_a, shared_b, shared_c, n, first, last);
    printf("Nodo %d: filas [%u, %u) de C calculadas.\n", node_id, first, last);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 2);

    double* expected = malloc(bytes);
    multiply_rows(a, b, expected, n, 0, n);
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
    app_print_stats(node_id);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 3);

    app_check(dsm_free(segment), "dsm_free");
    free(a);
    free(b);
    free(expected);
    return wrong == 0 ? 0 : 1;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> <matriz_a> <matriz_b>\n", argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = matmul_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
