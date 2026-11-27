/* Aplicación de prueba 1: conteo distribuido de palabras, líneas y letras.
 *
 * En el nodo origen: run wordcount <archivo> (texto de hasta la memoria del sistema).
 * Los demás nodos la reciben como run wordcount, sin archivo.
 *
 * 1. El origen lee el archivo, lo copia al segmento compartido y publica su largo.
 *    Si el texto no cabe en un nodo, el segmento sigue en la memoria de otros nodos.
 * 2. Barrera. Cada nodo cuenta su tramo con varios pthreads, leyendo el texto a
 *    través del DSM, y deja el resultado en su página de control.
 * 3. Barrera. Todos suman los resultados; el origen los compara con el conteo
 *    secuencial, los guarda en output/wordcount.txt y publica el veredicto.
 * 4. Barrera y dsm_free; el segmento se libera cuando lo suelta el último nodo.
 */
#include "app_common.h"
#include <string.h>

#define PROC_WORDCOUNT 10

/* Página de control de cada nodo. phase va primero: lo usa app_barrier. */
typedef struct {
    uint32_t phase;
    uint32_t reserved;
    uint64_t words;
    uint64_t lines;
    uint64_t letters[26];
} result_t;

/* Bytes de una palabra: letras ASCII y bytes UTF-8 (tildes, ñ). */
static int word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

/* Cuenta el tramo [begin, end) de text. Una palabra cuenta en el tramo donde empieza. */
static void count(const volatile char* text, uint64_t begin, uint64_t end, result_t* out) {
    out->words = out->lines = 0;
    memset(out->letters, 0, sizeof(out->letters));
    for (uint64_t i = begin; i < end; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (word_byte(c) && (i == 0 || !word_byte((unsigned char)text[i - 1]))) {
            ++out->words;
        }
        if (c == '\n') {
            ++out->lines;
        } else if (c >= 'a' && c <= 'z') {
            ++out->letters[c - 'a'];
        } else if (c >= 'A' && c <= 'Z') {
            ++out->letters[c - 'A'];
        }
    }
}

static char* read_file(const char* path, uint64_t* length) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        perror(path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    *length = (uint64_t)ftell(file);
    rewind(file);
    char* data = malloc(*length + 1);
    if (data == NULL || fread(data, 1, *length, file) != *length) {
        fprintf(stderr, "No se pudo leer %s\n", path);
        free(data);
        data = NULL;
    }
    fclose(file);
    return data;
}

typedef struct {
    const char* text;
    result_t results[APP_THREADS];
} count_job_t;

/* Tarea de un pthread: cuenta su pedazo del tramo en su propio resultado. */
static void count_task(void* context, uint64_t begin, uint64_t end, int thread) {
    count_job_t* job = context;
    count(job->text, begin, end, &job->results[thread]);
}

static void add_result(result_t* total, const result_t* part) {
    total->words += part->words;
    total->lines += part->lines;
    for (int l = 0; l < 26; ++l) {
        total->letters[l] += part->letters[l];
    }
}

/* Corre la aplicación en el nodo ya iniciado de este proceso. Con argumento este
   nodo es el origen; sin argumentos toma el texto del DSM. */
int wordcount_run(int node_id, int argc, char* argv[]) {
    const int origin = argc >= 1;
    const int nodes = app_node_count();
    app_wait_all_online();
    volatile app_info_t* info = app_info_join(PROC_WORDCOUNT, nodes);
    double start = app_now_ms();
    const size_t page = dsm_page_size();

    char* local = NULL;
    uint64_t length = 0;
    if (origin) {
        local = read_file(argv[0], &length);
        if (local != NULL && length + nodes * page > app_system_bytes()) {
            fprintf(stderr, "El texto ocupa %llu KB y el sistema tiene %zu KB entre todos "
                            "los nodos.\n",
                    (unsigned long long)length / 1024, app_system_bytes() / 1024);
            free(local);
            local = NULL;
        }
        if (local == NULL || length == 0) {
            app_info_cancel(info, node_id, nodes);
            dsm_free((void*)info);
            free(local);
            return 1;
        }
        info->params[0] = length;
        app_info_publish(info, node_id);
    } else {
        if (!app_info_wait(info, node_id)) {
            dsm_free((void*)info);
            return 1;
        }
        length = info->params[0];
    }

    start = app_now_ms(); /* La carga se mide desde que la entrada está publicada. */
    char* segment = app_join(PROC_WORDCOUNT, nodes * page + length, node_id);
    if (segment == NULL) {
        dsm_free((void*)info);
        free(local);
        return 1;
    }
    char* text = segment + nodes * page;
    if (origin) {
        memcpy(text, local, length);
        printf("Nodo %d: texto de %llu bytes cargado en el DSM (%p).\n", node_id,
               (unsigned long long)length, (void*)segment);
        fflush(stdout);
    }
    app_barrier(segment, node_id, nodes, 1);
    const double loaded = app_now_ms();

    const uint64_t begin = length * node_id / nodes;
    const uint64_t end = length * (node_id + 1) / nodes;
    count_job_t job;
    memset(&job, 0, sizeof(job));
    job.text = text;
    app_parallel(count_task, &job, begin, end);
    result_t* mine = (result_t*)(segment + node_id * page);
    result_t partial;
    memset(&partial, 0, sizeof(partial));
    for (int t = 0; t < APP_THREADS; ++t) {
        add_result(&partial, &job.results[t]);
    }
    mine->words = partial.words;
    mine->lines = partial.lines;
    memcpy(mine->letters, partial.letters, sizeof(partial.letters));
    printf("Nodo %d: bytes [%llu, %llu) contados con %d pthreads: %llu palabras.\n", node_id,
           (unsigned long long)begin, (unsigned long long)end, APP_THREADS,
           (unsigned long long)partial.words);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 2);
    const double computed = app_now_ms();

    result_t total;
    memset(&total, 0, sizeof(total));
    for (int n = 0; n < nodes; ++n) {
        add_result(&total, (const result_t*)(segment + n * page));
    }
    double sequential = -1;
    if (origin) {
        result_t expected;
        const double sequential_start = app_now_ms();
        count(local, 0, length, &expected);
        sequential = app_now_ms() - sequential_start;
        const int ok = total.words == expected.words && total.lines == expected.lines &&
                       memcmp(total.letters, expected.letters, sizeof(total.letters)) == 0;
        char path[512];
        FILE* output = app_output("wordcount", ".txt", path, sizeof(path));
        if (output != NULL) {
            fprintf(output, "archivo %s\npalabras %llu\nlineas %llu\n", argv[0],
                    (unsigned long long)total.words, (unsigned long long)total.lines);
            for (int l = 0; l < 26; ++l) {
                fprintf(output, "%c %llu\n", 'a' + l, (unsigned long long)total.letters[l]);
            }
            fclose(output);
            printf("Nodo %d: conteo guardado en %s\n", node_id, path);
        }
        printf("Nodo %d: verificación contra el conteo secuencial: %s\n", node_id,
               ok ? "CORRECTA" : "ERROR");
        info->verdict = ok ? APP_VERDICT_OK : APP_VERDICT_ERROR;
    }
    printf("Nodo %d: %llu palabras, %llu líneas; letras más frecuentes:", node_id,
           (unsigned long long)total.words, (unsigned long long)total.lines);
    for (int shown = 0; shown < 5; ++shown) {
        int best = 0;
        for (int l = 1; l < 26; ++l) {
            if (total.letters[l] > total.letters[best]) {
                best = l;
            }
        }
        printf(" %c=%llu", 'a' + best, (unsigned long long)total.letters[best]);
        total.letters[best] = 0;
    }
    printf("\n");
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
    free(local);
    return verdict == APP_VERDICT_OK ? 0 : 1;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> [archivo (nodo origen)]\n", argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = wordcount_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
