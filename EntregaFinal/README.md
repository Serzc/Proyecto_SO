# Sistema DSM (IC-6600, II Semestre 2026)

Memoria compartida distribuida en espacio de usuario para Linux, con paginación bajo demanda y coherencia MSI.
Autores: Sergio Andrés Zúñiga Castillo y José Ignacio Madriz Ortiz.
Repositorio: https://github.com/Serzc/Proyecto_SO

Documentación en `docs/`:

- `SO.2026 - Proyecto Entrega Final - Informe tecnico.pdf` (y `.docx` editable): arquitectura, cambios desde el Entregable 1, MSI, protocolo, concurrencia, API, aplicaciones y resultados.
- `demo.md`: guion de la demostración en el laboratorio.
- `figures/`: diagramas del informe (fuente Mermaid `.mmd` e imagen `.png`).

## Requisitos

- Linux x86_64, kernel 4.17 o posterior.
- `g++` (C++17), `gcc` y `make`.
- Los puertos TCP de la configuración abiertos entre todas las máquinas.

## Compilar y probar

```
make          # libdsm.a, dsmnode, wordcount, matmul, imgfilter y las pruebas
make test     # pruebas automáticas en una sola máquina
make bench    # tiempos de las tres apps con 1, 2 y 3 nodos locales
make clean
```

## Configuración (`dsm.conf`)

Todas las máquinas usan el mismo archivo. El orden de las líneas `host` define el `node_id`.

| Opción | Por defecto | Uso |
|---|---|---|
| `page_size` | 4096 | Tamaño de página en bytes (múltiplo de 4096) |
| `pages_per_node` | 256 | Páginas que aporta cada nodo |
| `heartbeat_ms` | 1000 | Periodo del heartbeat |
| `timeout_ms` | 5000 | Tiempo para dar un nodo por caído o una solicitud por perdida |
| `port_base` | 28400 | El nodo k usa los puertos `port_base + 3k` (datos), `+1` (control) y `+2` (heartbeat) |
| `dsm_base` | 0x100000000000 | Dirección donde empieza el espacio DSM |
| `host` | | IPv4 de cada nodo, una línea por nodo (máximo 32) |

Para varios nodos en una sola máquina se repite `127.0.0.1` (ejemplo: `apps/local3.conf`). El `JOIN` de cada nodo lleva estos parámetros y la lista de hosts; un nodo con un `dsm.conf` distinto es rechazado, queda CAIDO para los demás y `monitor` lo avisa.

## Uso

En cada máquina se inicia un nodo con su consola:

```
./dsmnode dsm.conf <id>
```

| Comando | Qué hace |
|---|---|
| `nodes` | Nodos configurados, su estado y sus puertos |
| `stats` | Uso del sistema: páginas totales y libres, copias M/S/I, fallos y transferencias por nodo |
| `pages` | Vista interactiva por nodo (flechas para cambiar de nodo, `q` o Enter sale): memoria usada, mapa de páginas por proceso, asignaciones y log de solicitudes; se redibuja sola |
| `pages <id>` | Páginas asignadas cuyo home es el nodo `id` |
| `pages all` | Tabla de páginas de todo el sistema |
| `run <app> <archivos>` | Lanza una aplicación en todos los nodos (ver abajo) |
| `monitor` | Mensajes recibidos y cambios de estado de los nodos (Enter sale) |
| `help`, `quit` | Ayuda; salir (espera a las aplicaciones en curso) |

Las aplicaciones se lanzan desde la consola de **un solo nodo**, el que tiene los archivos de entrada:

```
run matmul data/matriz_a.txt data/matriz_b.txt
run wordcount data/enunciado_dsm.txt
run imgfilter data/aldrin.ppm
```

Ese nodo es el origen: lee los archivos, copia la entrada al DSM y envía `RUN_REQUEST` a los demás, que ejecutan la misma app sin archivos y leen la entrada a través del DSM. Todos calculan su parte con 4 pthreads. El origen compara el resultado con un cálculo secuencial, imprime `CORRECTA` o `ERROR` y lo guarda en su `output/`: `matmul_C.txt`, `wordcount.txt` o `<imagen>_lapiz.pgm`. Todos los nodos de `dsm.conf` deben estar en línea. `imgfilter` necesita `pages_per_node = 1024` y al menos dos nodos para la foto de prueba (6.4 MB). Otras fotos se convierten a PPM con `convert foto.jpg foto.ppm` (ImageMagick).

Las aplicaciones también existen como programas que inician su propio nodo (las usa `bench.sh`): en el origen `./matmul dsm.conf <id> <a> <b>`, `./wordcount dsm.conf <id> <archivo>` o `./imgfilter dsm.conf <id> <imagen.ppm>`, y en los demás nodos el mismo programa sin archivos.

## Guía del API (`dsm.h`)

Un programa incluye `dsm.h` y se enlaza con `g++` porque la biblioteca es C++: `gcc -c app.c && g++ app.o libdsm.a -pthread -o app` (así lo hace el `Makefile` con las apps). Todas las funciones devuelven 0 o un valor positivo si tienen éxito, y un código negativo `DSM_E_*` si fallan.

```c
#include <stdio.h>
#include "dsm.h"

int main(void) {
    if (dsm_init("dsm.conf", 0) != DSM_OK) return 1;   /* este proceso es el nodo 0 */

    double* v;
    int rc = dsm_alloc(1000 * sizeof(double), 42, (void**)&v);  /* segmento 42 */
    if (rc != DSM_OK) { fprintf(stderr, "%s\n", dsm_strerror(rc)); return 1; }

    v[0] = 3.14;          /* acceso normal: los fallos de página los resuelve el DSM */

    dsm_free(v);          /* se libera cuando lo suelta el último nodo unido */
    dsm_shutdown();
    return 0;
}
```

| Función | Uso | Errores |
|---|---|---|
| `int dsm_init(const char* config, int node_id)` | Inicia el nodo: lee la configuración, reserva la región, instala el manejador de `SIGSEGV` y arranca los hilos. Una vez por proceso | `INVAL`, `CONFIG`, `NET`, `NOMEM` |
| `void dsm_shutdown(void)` | Detiene el nodo. Espera hasta `timeout_ms` a que ningún nodo en línea use segmentos cuyo home es este nodo | |
| `int dsm_alloc(size_t size, uint16_t proc, void** ptr)` | Une el nodo al segmento `proc` (1 a 65535) y deja su dirección en `*ptr`. El mismo `proc` da la misma región en cualquier nodo; el tamaño se redondea a páginas. Si no cabe en un nodo, ocupa varios | `INVAL`, `NOMEM`, `NODE_DOWN`, `TIMEOUT`, `NET` |
| `int dsm_free(void* ptr)` | Separa el nodo del segmento; el último nodo unido lo libera | `BADADDR`, `INVAL`, `TIMEOUT` |
| `int dsm_nodes(dsm_node_t* out, int max)` | Copia la tabla de nodos (IPv4, rango de páginas, estado). Devuelve cuántos copió | `INVAL` |
| `int dsm_stats(int node_id, dsm_stats_t* out)` | Estadísticas de cualquier nodo en línea | `INVAL`, `NODE_DOWN`, `TIMEOUT` |
| `int dsm_page_table(int node_id, uint32_t first, uint32_t count, dsm_page_t* out)` | Hasta 256 entradas del directorio de un home: proceso, orden, dueño, estado y copias. Devuelve cuántas copió | `INVAL`, `NODE_DOWN`, `TIMEOUT` |
| `void* dsm_page_address(uint32_t page)` | Dirección de una página global, o `NULL` | |
| `size_t dsm_page_size(void)` | Tamaño de página configurado | |
| `const char* dsm_strerror(int err)` | Texto de un código de error | |

Códigos: `DSM_OK` 0, `DSM_E_INVAL` -1 (argumento inválido o nodo sin iniciar), `DSM_E_CONFIG` -2 (configuración inválida), `DSM_E_NOMEM` -3 (no hay memoria en el sistema), `DSM_E_BADADDR` -4 (dirección que no es un segmento), `DSM_E_NODE_DOWN` -5 (nodo caído), `DSM_E_TIMEOUT` -6 (sin respuesta en `timeout_ms`), `DSM_E_NET` -7 (error de red). `DSM_E_NOT_IMPLEMENTED` -8 está reservado.

Reglas de uso:

- Los punteros a memoria DSM valen en todos los nodos: la región empieza en la misma dirección en todos.
- Para sincronizar nodos no hay locks distribuidos: se usan banderas en páginas compartidas, como las barreras de `apps/app_common.h`. Cada nodo escribe solo su propia bandera y lee las de los demás.
- Acceder a memoria DSM no asignada termina el proceso con `SIGSEGV`, como un puntero inválido.

## Arquitectura

Cada proceso que llama a `dsm_init` es un nodo par; no hay servidor ni coordinador. Todos los nodos proyectan el mismo rango de direcciones con `mmap`, con las páginas en `PROT_NONE`. Al tocar una página sin permiso ocurre un `SIGSEGV`. El manejador pasa la página a un hilo de fallos, que la pide por TCP a su home y aplica `mprotect` según el estado MSI (I sin acceso, S lectura, M lectura y escritura). El home guarda un directorio con el dueño y las copias de cada página, invalida las copias antes de conceder escritura y recupera las páginas modificadas (write-back).

| Módulo | Contenido |
|---|---|
| `pager.cpp` | Memoria, `SIGSEGV`, fallos y directorio MSI |
| `network.cpp` | TCP, protocolo de mensajes, JOIN, heartbeat |
| `node.cpp` | Ciclo de vida del nodo, segmentos y consultas |
| `config.cpp`, `protocol.cpp` | Configuración y codificación de mensajes |
| `dsm.cpp`, `dsm.h` | API en C |
| `main.cpp` | Consola `dsmnode` |
| `apps/` | `wordcount`, `matmul`, `imgfilter` y utilidades comunes |
| `test_memoria.cpp`, `test_coherencia.cpp` | Pruebas automáticas |
