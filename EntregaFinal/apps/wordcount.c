/* Aplicación de prueba 1: conteo distribuido de palabras, líneas y letras.
 *
 * Uso: wordcount <dsm.conf> <node_id> [archivo]
 *   El nodo 0 coordina: carga el archivo de texto en el DSM y verifica el
 *   resultado contra un conteo secuencial. Cada uno de los demás nodos cuenta un
 *   tramo del texto, leyendo las páginas por la red, y escribe su resultado en su
 *   propia página del DSM.
 */
#include "app_common.h"
#include <string.h>

#define PROC_TEXT 10
#define PROC_CONTROL 11

enum { PHASE_WAIT = 0, PHASE_READY = 1, PHASE_EXIT = 2 };

typedef struct {
    uint32_t phase;
    uint32_t workers;
    uint64_t text_len;
} control_t;

typedef struct {
    uint32_t done;
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
    memset(out, 0, sizeof(*out));
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
        exit(1);
    }
    fseek(file, 0, SEEK_END);
    *length = (uint64_t)ftell(file);
    rewind(file);
    char* data = malloc(*length);
    if (data == NULL || fread(data, 1, *length, file) != *length) {
        fprintf(stderr, "No se pudo leer %s\n", path);
        exit(1);
    }
    fclose(file);
    return data;
}

static int coordinator(const char* path) {
    const size_t page = dsm_page_size();
    const uint32_t workers = (uint32_t)app_node_count() - 1;
    if (workers == 0) {
        fprintf(stderr, "Se necesita al menos un nodo trabajador.\n");
        exit(1);
    }
    uint64_t length = 0;
    char* local = read_file(path, &length);
    app_wait_all_online();

    char* text = NULL;
    control_t* control = NULL;
    app_check(dsm_alloc(length, PROC_TEXT, (void**)&text), "dsm_alloc texto");
    app_check(dsm_alloc((1 + workers) * page, PROC_CONTROL, (void**)&control),
              "dsm_alloc control");
    memcpy(text, local, length);
    control->workers = workers;
    control->text_len = length;
    ((volatile control_t*)control)->phase = PHASE_READY;
    printf("Texto de %llu bytes cargado en el DSM en %p; %u trabajadores.\n",
           (unsigned long long)length, (void*)text, workers);
    fflush(stdout);

    result_t expected;
    count(local, 0, length, &expected);

    result_t total;
    memset(&total, 0, sizeof(total));
    for (uint32_t w = 0; w < workers; ++w) {
        result_t* result = (result_t*)((char*)control + (1 + w) * page);
        app_wait_value(&result->done, 1);
        total.words += result->words;
        total.lines += result->lines;
        for (int l = 0; l < 26; ++l) {
            total.letters[l] += result->letters[l];
        }
        printf("Trabajador %u: %llu palabras\n", w + 1, (unsigned long long)result->words);
    }

    const int ok = total.words == expected.words && total.lines == expected.lines &&
                   memcmp(total.letters, expected.letters, sizeof(total.letters)) == 0;
    printf("\nArchivo: %s\nPalabras: %llu\nLíneas: %llu\nLetras más frecuentes:",
           path, (unsigned long long)total.words, (unsigned long long)total.lines);
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
    printf("\nVerificación contra el conteo secuencial: %s\n", ok ? "CORRECTA" : "ERROR");
    app_print_stats(0);

    app_check(dsm_free(text), "dsm_free texto");
    ((volatile control_t*)control)->phase = PHASE_EXIT;
    usleep(500 * 1000);
    app_check(dsm_free(control), "dsm_free control");
    free(local);
    return ok ? 0 : 1;
}

static void worker(int node_id) {
    const size_t page = dsm_page_size();
    printf("Trabajador %d: esperando datos del coordinador...\n", node_id);
    fflush(stdout);
    volatile control_t* control = app_wait_find(PROC_CONTROL);
    app_wait_value(&control->phase, PHASE_READY);
    const char* text = app_wait_find(PROC_TEXT);

    const uint64_t length = control->text_len;
    const uint32_t workers = control->workers;
    const uint32_t index = (uint32_t)node_id - 1;
    const uint64_t begin = length * index / workers;
    const uint64_t end = length * (index + 1) / workers;
    result_t local;
    count(text, begin, end, &local);

    result_t* result = (result_t*)((char*)control + (1 + index) * page);
    memcpy((char*)result + sizeof(uint64_t), (char*)&local + sizeof(uint64_t),
           sizeof(local) - sizeof(uint64_t));
    ((volatile result_t*)result)->done = 1;
    printf("Trabajador %d: bytes [%llu, %llu): %llu palabras\n", node_id,
           (unsigned long long)begin, (unsigned long long)end,
           (unsigned long long)local.words);
    app_wait_value(&control->phase, PHASE_EXIT);
    app_print_stats(node_id);
    sleep(2); /* El coordinador libera el bloque de control mientras seguimos en línea. */
}

/* Corre la aplicación en el nodo ya iniciado de este proceso. Lo usan el
   comando run de dsmnode y el main de abajo. En el nodo 0 recibe <archivo>. */
int wordcount_run(int node_id, int argc, char* argv[]) {
    if (node_id == 0) {
        if (argc < 1) {
            fprintf(stderr, "Uso en el nodo 0: wordcount <archivo>\n");
            return 2;
        }
        return coordinator(argv[0]);
    }
    worker(node_id);
    return 0;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> [<archivo> (solo nodo 0)]\n", argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = wordcount_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
