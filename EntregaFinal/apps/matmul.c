/* Aplicación de prueba 2: multiplicación distribuida de matrices C = A x B.
 *
 * Uso: matmul <dsm.conf> <node_id> [matriz_a matriz_b]
 *   El nodo 0 coordina: lee A y B (formato: n y luego n*n números), las copia al
 *   DSM y compara C con la multiplicación secuencial. Cada uno de los demás nodos
 *   calcula un bloque de filas de C. A y B se leen como páginas compartidas (S) y
 *   las filas de C se escriben con páginas en M, así que los bloques que comparten
 *   una página fuerzan invalidaciones entre nodos.
 */
#include "app_common.h"
#include <string.h>

#define PROC_INPUT 20
#define PROC_CONTROL 21
#define PROC_OUTPUT 22

enum { PHASE_WAIT = 0, PHASE_READY = 1, PHASE_EXIT = 2 };

typedef struct {
    uint32_t phase;
    uint32_t workers;
    uint32_t n;
} control_t;

static double* read_matrix(const char* path, uint32_t* n) {
    FILE* file = fopen(path, "r");
    if (file == NULL || fscanf(file, "%u", n) != 1 || *n == 0 || *n > 128) {
        fprintf(stderr, "%s: formato inválido (n entre 1 y 128)\n", path);
        exit(1);
    }
    double* matrix = malloc(sizeof(double) * *n * *n);
    for (uint32_t i = 0; i < *n * *n; ++i) {
        if (fscanf(file, "%lf", &matrix[i]) != 1) {
            fprintf(stderr, "%s: faltan valores\n", path);
            exit(1);
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

static int coordinator(const char* path_a, const char* path_b) {
    const size_t page = dsm_page_size();
    const uint32_t workers = (uint32_t)app_node_count() - 1;
    if (workers == 0) {
        fprintf(stderr, "Se necesita al menos un nodo trabajador.\n");
        exit(1);
    }
    uint32_t n = 0;
    uint32_t n_b = 0;
    double* a = read_matrix(path_a, &n);
    double* b = read_matrix(path_b, &n_b);
    if (n != n_b) {
        fprintf(stderr, "Las matrices deben tener el mismo tamaño.\n");
        exit(1);
    }
    app_wait_all_online();

    const size_t bytes = sizeof(double) * n * n;
    double* input = NULL;
    double* output = NULL;
    control_t* control = NULL;
    app_check(dsm_alloc(2 * bytes, PROC_INPUT, (void**)&input), "dsm_alloc A y B");
    app_check(dsm_alloc(bytes, PROC_OUTPUT, (void**)&output), "dsm_alloc C");
    app_check(dsm_alloc((1 + workers) * page, PROC_CONTROL, (void**)&control),
              "dsm_alloc control");
    memcpy(input, a, bytes);
    memcpy(input + n * n, b, bytes);
    control->workers = workers;
    control->n = n;
    ((volatile control_t*)control)->phase = PHASE_READY;
    printf("Matrices %ux%u cargadas en el DSM (A y B en %p, C en %p); %u trabajadores.\n",
           n, n, (void*)input, (void*)output, workers);
    fflush(stdout);

    double* expected = malloc(bytes);
    multiply_rows(a, b, expected, n, 0, n);
    for (uint32_t w = 0; w < workers; ++w) {
        app_wait_value((volatile uint32_t*)((char*)control + (1 + w) * page), 1);
        printf("Trabajador %u terminó sus filas.\n", w + 1);
    }

    uint32_t wrong = 0;
    double checksum = 0;
    for (uint32_t i = 0; i < n * n; ++i) {
        wrong += output[i] != expected[i];
        checksum += output[i];
    }
    printf("\nC[0][0] = %.4f, C[%u][%u] = %.4f, suma de C = %.4f\n", output[0], n - 1,
           n - 1, output[n * n - 1], checksum);
    printf("Verificación contra la multiplicación secuencial: %s (%u de %u celdas distintas)\n",
           wrong == 0 ? "CORRECTA" : "ERROR", wrong, n * n);
    app_print_stats(0);

    app_check(dsm_free(input), "dsm_free A y B");
    app_check(dsm_free(output), "dsm_free C");
    ((volatile control_t*)control)->phase = PHASE_EXIT;
    usleep(500 * 1000);
    app_check(dsm_free(control), "dsm_free control");
    free(a);
    free(b);
    free(expected);
    return wrong == 0 ? 0 : 1;
}

static void worker(int node_id) {
    const size_t page = dsm_page_size();
    printf("Trabajador %d: esperando datos del coordinador...\n", node_id);
    fflush(stdout);
    volatile control_t* control = app_wait_find(PROC_CONTROL);
    app_wait_value(&control->phase, PHASE_READY);
    const double* input = app_wait_find(PROC_INPUT);
    double* output = app_wait_find(PROC_OUTPUT);

    const uint32_t n = control->n;
    const uint32_t index = (uint32_t)node_id - 1;
    const uint32_t first = n * index / control->workers;
    const uint32_t last = n * (index + 1) / control->workers;
    multiply_rows(input, input + n * n, output, n, first, last);
    *(volatile uint32_t*)((char*)control + (1 + index) * page) = 1;
    printf("Trabajador %d: filas [%u, %u) calculadas.\n", node_id, first, last);
    app_wait_value(&control->phase, PHASE_EXIT);
    app_print_stats(node_id);
    sleep(2); /* El coordinador libera el bloque de control mientras seguimos en línea. */
}

/* Corre la aplicación en el nodo ya iniciado de este proceso. Lo usan el
   comando run de dsmnode y el main de abajo. En el nodo 0 recibe <matriz_a> <matriz_b>. */
int matmul_run(int node_id, int argc, char* argv[]) {
    if (node_id == 0) {
        if (argc < 2) {
            fprintf(stderr, "Uso en el nodo 0: matmul <matriz_a> <matriz_b>\n");
            return 2;
        }
        return coordinator(argv[0], argv[1]);
    }
    worker(node_id);
    return 0;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> [<matriz_a> <matriz_b> (solo nodo 0)]\n", argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = matmul_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
