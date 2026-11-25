/* Aplicación de prueba 1: conteo distribuido de palabras, líneas y letras.
 *
 * Todos los nodos son iguales y ejecutan lo mismo: run wordcount <archivo>
 * (el mismo archivo de texto en todas las máquinas, hasta ~1 MB).
 *
 * 1. Cada nodo copia al segmento compartido su tramo del texto.
 * 2. Barrera. Cada nodo cuenta el tramo que cargó el nodo siguiente, así siempre
 *    lee páginas escritas por otro, y deja su resultado en su página de control.
 * 3. Barrera. Cada nodo suma los resultados de todos y los compara con el conteo
 *    secuencial de su copia local del archivo; todos imprimen lo mismo.
 * 4. Barrera y dsm_free; el segmento se libera cuando lo suelta el último nodo.
 */
#include "app_common.h"
#include <string.h>

#define PROC_WORDCOUNT 10

/* Página de control de cada nodo. phase va primero: lo usa app_barrier. */
typedef struct {
    uint32_t phase;
    uint32_t chunk;
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

/* Corre la aplicación en el nodo ya iniciado de este proceso. La usan el comando
   run de dsmnode y el main de abajo. */
int wordcount_run(int node_id, int argc, char* argv[]) {
    if (argc < 1) {
        fprintf(stderr, "Uso: wordcount <archivo>\n");
        return 2;
    }
    uint64_t length = 0;
    char* local = read_file(argv[0], &length);
    if (local == NULL) {
        return 1;
    }

    const int nodes = app_node_count();
    app_wait_all_online();
    const size_t page = dsm_page_size();
    char* segment = app_join(PROC_WORDCOUNT, nodes * page + length, node_id);
    if (segment == NULL) {
        free(local);
        return 1;
    }
    char* text = segment + nodes * page;
    result_t* mine = (result_t*)(segment + node_id * page);

    const uint64_t begin = length * node_id / nodes;
    const uint64_t end = length * (node_id + 1) / nodes;
    memcpy(text + begin, local + begin, end - begin);
    printf("Nodo %d: bytes [%llu, %llu) del texto cargados en el DSM (%p).\n", node_id,
           (unsigned long long)begin, (unsigned long long)end, (void*)segment);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 1);

    const int chunk = (node_id + 1) % nodes;
    result_t partial;
    count(text, length * chunk / nodes, length * (chunk + 1) / nodes, &partial);
    mine->chunk = (uint32_t)chunk;
    mine->words = partial.words;
    mine->lines = partial.lines;
    memcpy(mine->letters, partial.letters, sizeof(partial.letters));
    printf("Nodo %d: contó el tramo del nodo %d: %llu palabras.\n", node_id, chunk,
           (unsigned long long)partial.words);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 2);

    result_t total;
    memset(&total, 0, sizeof(total));
    for (int n = 0; n < nodes; ++n) {
        const result_t* other = (const result_t*)(segment + n * page);
        total.words += other->words;
        total.lines += other->lines;
        for (int l = 0; l < 26; ++l) {
            total.letters[l] += other->letters[l];
        }
    }
    result_t expected;
    count(local, 0, length, &expected);
    const int ok = total.words == expected.words && total.lines == expected.lines &&
                   memcmp(total.letters, expected.letters, sizeof(total.letters)) == 0;
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
    printf("\nNodo %d: verificación contra el conteo secuencial: %s\n", node_id,
           ok ? "CORRECTA" : "ERROR");
    app_print_stats(node_id);
    fflush(stdout);
    app_barrier(segment, node_id, nodes, 3);

    app_check(dsm_free(segment), "dsm_free");
    free(local);
    return ok ? 0 : 1;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> <archivo>\n", argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = wordcount_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
