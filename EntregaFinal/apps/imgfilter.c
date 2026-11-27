/* Aplicación de prueba 3: filtro de imagen "dibujo a lápiz" en blanco y negro.
 *
 * En el nodo origen: run imgfilter <imagen.ppm> (PPM binario P6). Los demás nodos la
 * reciben como run imgfilter, sin archivo.
 *
 * La imagen se reparte en bandas de filas, una por nodo, y cada banda es su propio
 * segmento compartido con su home en un nodo distinto. Así la imagen puede ser más
 * grande que la memoria que aporta un solo nodo.
 *
 * 1. El origen lee la imagen, copia las bandas RGB a los segmentos de entrada y
 *    publica el ancho y el alto.
 * 2. Barrera. Cada nodo calcula su banda de salida con varios pthreads: escala de
 *    grises y bordes con Sobel, invertidos (líneas oscuras sobre blanco). Lee la
 *    entrada por el DSM, incluidas las filas vecinas de las bandas de otros nodos.
 * 3. Barrera. El origen compara la salida completa con el filtro secuencial, la guarda
 *    en output/<imagen>_lapiz.pgm y publica el veredicto.
 * 4. Barrera y dsm_free de todos los segmentos.
 */
#include "app_common.h"
#include <string.h>

#define PROC_IMG_CONTROL 30
#define PROC_IMG_INPUT 40   /* + banda */
#define PROC_IMG_OUTPUT 70  /* + banda */

typedef struct {
    uint32_t width;
    uint32_t height;
    int nodes;
    const volatile unsigned char* input[APP_MAX_NODES];  /* RGB por banda */
    volatile unsigned char* output[APP_MAX_NODES];       /* gris por banda */
    uint32_t first_row[APP_MAX_NODES + 1];
} image_t;

static unsigned char* read_ppm(const char* path, uint32_t* width, uint32_t* height) {
    FILE* file = fopen(path, "rb");
    unsigned max = 0;
    if (file == NULL || fscanf(file, "P6 %u %u %u", width, height, &max) != 3 ||
        max != 255 || *width == 0 || *height == 0 || fgetc(file) == EOF) {
        fprintf(stderr, "%s: no existe o no es un PPM P6 de 8 bits\n", path);
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    const size_t bytes = (size_t)*width * *height * 3;
    unsigned char* pixels = malloc(bytes);
    if (pixels == NULL || fread(pixels, 1, bytes, file) != bytes) {
        fprintf(stderr, "%s: imagen incompleta\n", path);
        free(pixels);
        pixels = NULL;
    }
    fclose(file);
    return pixels;
}

/* Gris de un píxel RGB (pesos ITU-R BT.601). */
static int gray(const volatile unsigned char* rgb) {
    return (299 * rgb[0] + 587 * rgb[1] + 114 * rgb[2]) / 1000;
}

/* Gris del píxel (x, y) de la imagen compartida; las coordenadas se recortan al borde. */
static int shared_gray(const image_t* img, int x, int y) {
    x = x < 0 ? 0 : x >= (int)img->width ? (int)img->width - 1 : x;
    y = y < 0 ? 0 : y >= (int)img->height ? (int)img->height - 1 : y;
    int band = 0;
    while ((uint32_t)y >= img->first_row[band + 1]) {
        ++band;
    }
    const size_t offset = ((size_t)(y - img->first_row[band]) * img->width + x) * 3;
    return gray(img->input[band] + offset);
}

static int local_gray(const unsigned char* rgb, uint32_t width, uint32_t height, int x,
                      int y) {
    x = x < 0 ? 0 : x >= (int)width ? (int)width - 1 : x;
    y = y < 0 ? 0 : y >= (int)height ? (int)height - 1 : y;
    return gray(rgb + ((size_t)y * width + x) * 3);
}

/* Sobel sobre la vecindad 3x3 (g[fila][columna]); devuelve el píxel "a lápiz". */
static unsigned char sketch(int g[3][3]) {
    const int gx = (g[0][2] + 2 * g[1][2] + g[2][2]) - (g[0][0] + 2 * g[1][0] + g[2][0]);
    const int gy = (g[2][0] + 2 * g[2][1] + g[2][2]) - (g[0][0] + 2 * g[0][1] + g[0][2]);
    int magnitude = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
    magnitude = magnitude > 255 ? 255 : magnitude;
    return (unsigned char)(255 - magnitude);
}

/* Guarda la imagen de salida (PGM P5) en output/<imagen>_lapiz.pgm. */
static int write_pgm(const char* input_path, const image_t* img, char* path,
                     size_t path_size) {
    const char* slash = strrchr(input_path, '/');
    const char* base = slash != NULL ? slash + 1 : input_path;
    const char* dot = strrchr(base, '.');
    char name[256];
    snprintf(name, sizeof(name), "%.*s_lapiz",
             dot != NULL ? (int)(dot - base) : (int)strlen(base), base);
    FILE* file = app_output(name, ".pgm", path, path_size);
    if (file == NULL) {
        return 0;
    }
    fprintf(file, "P5\n%u %u\n255\n", img->width, img->height);
    unsigned char row[8192];
    for (int band = 0; band < img->nodes; ++band) {
        for (uint32_t y = img->first_row[band]; y < img->first_row[band + 1]; ++y) {
            const size_t offset = (size_t)(y - img->first_row[band]) * img->width;
            for (uint32_t x = 0; x < img->width; ++x) {
                row[x] = img->output[band][offset + x];
            }
            fwrite(row, 1, img->width, file);
        }
    }
    fclose(file);
    return 1;
}

typedef struct {
    image_t* img;
    int node_id;
} filter_job_t;

/* Tarea de un pthread: aplica el filtro a las filas [begin, end) de la banda del nodo. */
static void filter_task(void* context, uint64_t begin, uint64_t end, int thread) {
    (void)thread;
    const filter_job_t* job = context;
    image_t* img = job->img;
    const uint32_t first = img->first_row[job->node_id];
    for (uint32_t y = (uint32_t)begin; y < (uint32_t)end; ++y) {
        for (uint32_t x = 0; x < img->width; ++x) {
            int g[3][3];
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    g[dy + 1][dx + 1] = shared_gray(img, (int)x + dx, (int)y + dy);
                }
            }
            img->output[job->node_id][(size_t)(y - first) * img->width + x] = sketch(g);
        }
    }
}

/* Corre la aplicación en el nodo ya iniciado de este proceso. Con argumento este
   nodo es el origen; sin argumentos toma la imagen del DSM. */
int imgfilter_run(int node_id, int argc, char* argv[]) {
    const int origin = argc >= 1;
    image_t img;
    memset(&img, 0, sizeof(img));
    img.nodes = app_node_count();
    app_wait_all_online();
    volatile app_info_t* info = app_info_join(PROC_IMG_CONTROL, img.nodes);
    double start = app_now_ms();

    unsigned char* local = NULL;
    if (origin) {
        local = read_ppm(argv[0], &img.width, &img.height);
        const size_t image_bytes = (size_t)img.width * img.height * 4;
        if (local != NULL && img.width > 8192) {
            fprintf(stderr, "La imagen puede tener como máximo 8192 píxeles de ancho.\n");
            free(local);
            local = NULL;
        } else if (local != NULL && image_bytes > app_system_bytes()) {
            /* Entrada RGB y salida en gris; una banda puede ocupar varios nodos. */
            fprintf(stderr, "La imagen necesita %zu KB y el sistema tiene %zu KB entre "
                            "todos los nodos: se necesitan más nodos o una imagen más "
                            "pequeña.\n",
                    image_bytes / 1024, app_system_bytes() / 1024);
            free(local);
            local = NULL;
        }
        if (local == NULL) {
            app_info_cancel(info, node_id, img.nodes);
            dsm_free((void*)info);
            return 1;
        }
        info->params[0] = img.width;
        info->params[1] = img.height;
        app_info_publish(info, node_id);
    } else {
        if (!app_info_wait(info, node_id)) {
            dsm_free((void*)info);
            return 1;
        }
        img.width = (uint32_t)info->params[0];
        img.height = (uint32_t)info->params[1];
    }

    start = app_now_ms(); /* La carga se mide desde que la entrada está publicada. */
    for (int band = 0; band <= img.nodes; ++band) {
        img.first_row[band] = (uint32_t)((uint64_t)img.height * band / img.nodes);
    }
    char* control = app_join(PROC_IMG_CONTROL, img.nodes * dsm_page_size(), node_id);
    if (control == NULL) {
        dsm_free((void*)info);
        free(local);
        return 1;
    }
    size_t total = 0;
    for (int band = 0; band < img.nodes; ++band) {
        const size_t rows = img.first_row[band + 1] - img.first_row[band];
        void* input = NULL;
        void* output = NULL;
        app_check(dsm_alloc(rows * img.width * 3, PROC_IMG_INPUT + band, &input),
                  "dsm_alloc banda de entrada");
        app_check(dsm_alloc(rows * img.width, PROC_IMG_OUTPUT + band, &output),
                  "dsm_alloc banda de salida");
        img.input[band] = input;
        img.output[band] = output;
        total += rows * img.width * 4;
    }
    if (origin) {
        for (int band = 0; band < img.nodes; ++band) {
            memcpy((void*)img.input[band],
                   local + (size_t)img.first_row[band] * img.width * 3,
                   (size_t)(img.first_row[band + 1] - img.first_row[band]) * img.width * 3);
        }
        printf("Nodo %d: imagen de %ux%u cargada en el DSM (%zu KB, %d segmentos).\n",
               node_id, img.width, img.height, total / 1024, 2 * img.nodes + 1);
        fflush(stdout);
    }
    app_barrier(control, node_id, img.nodes, 1);
    const double loaded = app_now_ms();

    const uint32_t first = img.first_row[node_id];
    const uint32_t last = img.first_row[node_id + 1];
    filter_job_t job = {&img, node_id};
    app_parallel(filter_task, &job, first, last);
    printf("Nodo %d: filtro aplicado a las filas [%u, %u) con %d pthreads.\n", node_id, first,
           last, APP_THREADS);
    fflush(stdout);
    app_barrier(control, node_id, img.nodes, 2);
    const double computed = app_now_ms();

    double sequential = -1;
    if (origin) {
        const double sequential_start = app_now_ms();
        unsigned char* expected = malloc((size_t)img.width * img.height);
        for (uint32_t y = 0; expected != NULL && y < img.height; ++y) {
            for (uint32_t x = 0; x < img.width; ++x) {
                int g[3][3];
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        g[dy + 1][dx + 1] = local_gray(local, img.width, img.height,
                                                       (int)x + dx, (int)y + dy);
                    }
                }
                expected[(size_t)y * img.width + x] = sketch(g);
            }
        }
        sequential = app_now_ms() - sequential_start;
        uint32_t wrong = expected == NULL ? img.width * img.height : 0;
        uint64_t checksum = 0;
        for (int band = 0; expected != NULL && band < img.nodes; ++band) {
            for (uint32_t y = img.first_row[band]; y < img.first_row[band + 1]; ++y) {
                for (uint32_t x = 0; x < img.width; ++x) {
                    const unsigned char value =
                        img.output[band][(size_t)(y - img.first_row[band]) * img.width + x];
                    wrong += value != expected[(size_t)y * img.width + x];
                    checksum += value;
                }
            }
        }
        free(expected);
        char path[512];
        const int saved = write_pgm(argv[0], &img, path, sizeof(path));
        printf("Nodo %d: suma de la salida = %llu; verificación contra el filtro secuencial: "
               "%s (%u de %u píxeles distintos)%s%s\n",
               node_id, (unsigned long long)checksum, wrong == 0 ? "CORRECTA" : "ERROR",
               wrong, img.width * img.height, saved ? "; guardada en " : "",
               saved ? path : "");
        info->verdict = wrong == 0 ? APP_VERDICT_OK : APP_VERDICT_ERROR;
    }
    app_print_times(node_id, start, loaded, computed, sequential);
    app_print_stats(node_id);
    fflush(stdout);
    app_barrier(control, node_id, img.nodes, 3);

    const uint32_t verdict = info->verdict;
    if (!origin) {
        printf("Nodo %d: verificación del nodo %u: %s\n", node_id, info->origin,
               app_verdict_name(verdict));
    }
    /* Si el home de un segmento ya terminó, esa memoria ya no existe: no es un error. */
    for (int band = 0; band < img.nodes; ++band) {
        dsm_free((void*)img.input[band]);
        dsm_free((void*)img.output[band]);
    }
    dsm_free(control);
    dsm_free((void*)info);
    free(local);
    return verdict == APP_VERDICT_OK ? 0 : 1;
}

#ifndef APP_NO_MAIN
int main(int argc, char* argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Uso: %s <dsm.conf> <node_id> [imagen.ppm (nodo origen)]\n", argv[0]);
        return 2;
    }
    const int node_id = atoi(argv[2]);
    app_check(dsm_init(argv[1], node_id), "dsm_init");
    const int result = imgfilter_run(node_id, argc - 3, argv + 3);
    dsm_shutdown();
    return result;
}
#endif
