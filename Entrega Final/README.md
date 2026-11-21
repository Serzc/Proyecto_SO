# Sistema DSM con paginación bajo demanda y coherencia MSI

Proyecto programado de IC-6600 Principios de Sistemas Operativos (TEC, II Semestre 2026).
Autores: Sergio Andrés Zúñiga Castillo y José Ignacio Madriz Ortiz.
Repositorio: https://github.com/Serzc/Proyecto_SO

Cada proceso que llama a `dsm_init` es un nodo. Los nodos reservan el mismo rango de
direcciones virtuales, y una página que no está en la memoria local se pide por TCP al nodo
home cuando el programa la toca. La coherencia sigue el protocolo MSI con un directorio en el
home de cada página.

## Requisitos

- Linux x86_64 con kernel 4.17 o posterior (`MAP_FIXED_NOREPLACE`, `memfd_create`).
- `g++` con C++17, `gcc` y `make`.
- Los puertos TCP de la configuración abiertos entre las máquinas.

## Compilar

```
make          # biblioteca libdsm.a, dsmnode, wordcount, matmul y las pruebas
make test     # pruebas automáticas en una sola máquina
make clean
```

## Configuración (`dsm.conf`)

Todas las máquinas usan el mismo archivo, con los hosts en el mismo orden. La posición de
cada línea `host` es el `node_id` de ese nodo.

| Parámetro | Valor por defecto | Significado |
|---|---|---|
| `page_size` | 4096 | Tamaño de página en bytes; debe coincidir con el de Linux |
| `pages_per_node` | 256 | Páginas que administra cada home (el máximo de una asignación) |
| `heartbeat_ms` | 1000 | Periodo del heartbeat |
| `timeout_ms` | 5000 | Silencio que marca un nodo como caído y espera máxima de una solicitud |
| `port_base` | 28400 | El nodo k usa `port_base + 3k` (datos), `+3k+1` (control) y `+3k+2` (heartbeat) |
| `dsm_base` | 0x100000000000 | Dirección virtual donde empieza el espacio DSM en todos los nodos |
| `host` | | IPv4 o nombre de cada nodo, una línea por nodo (máximo 32) |

Para varios nodos en la misma computadora se repite `127.0.0.1` (ver `apps/local3.conf`).
Al arrancar, un nodo verifica que la IPv4 de su `node_id` pertenezca a la máquina.

## Ejecutar

### Nodo de consola

```
./dsmnode dsm.conf <node_id>
```

| Comando | Qué muestra |
|---|---|
| `nodes` | Nodos configurados, estado (EN LINEA o CAIDO) y puertos |
| `stats` | Estadísticas de todos los nodos y del sistema: páginas libres, copias M/S/I, fallos, páginas enviadas y recibidas |
| `pages <node_id>` | Páginas asignadas con home en ese nodo: proceso, orden, dueño, estado y nodos con copia |
| `send <id> <data\|control> <texto>`, `broadcast <data\|control> <texto>` | Mensajes de prueba del transporte |
| `quit` | Cierra el nodo |

Las consultas a otros nodos usan `QUERY_REQUEST`, así que desde cualquier nodo se ve el
sistema completo.

### Aplicaciones de prueba

Las dos aplicaciones usan solo el API de `dsm.h`. El nodo 0 coordina y los demás calculan.
Se inicia un proceso por nodo, en cualquier orden; el coordinador espera a que todos estén
en línea.

Conteo de palabras, líneas y letras de un texto:

```
./wordcount dsm.conf 1                 # en cada trabajador (1, 2, ...)
./wordcount dsm.conf 0 data/enunciado_dsm.txt
```

Multiplicación de matrices (archivos con `n` y luego `n*n` números, `n <= 128`):

```
./matmul dsm.conf 1                    # en cada trabajador
./matmul dsm.conf 0 data/matriz_a.txt data/matriz_b.txt
```

El coordinador compara el resultado con un cálculo secuencial e imprime `CORRECTA` o `ERROR`.
Cada nodo imprime sus fallos de página y las páginas que envió y recibió.

Para probar en una sola máquina, use `apps/local3.conf` y abra tres terminales.

## Módulos

| Archivo | Contenido |
|---|---|
| `dsm.h` | API pública en C |
| `dsm_internal.h` | Tipos, constantes, `NodeTable` y la declaración de `Node` |
| `config.cpp` | Lectura de `dsm.conf`, resolución de hosts y puertos |
| `protocol.cpp` | Codificación de encabezados y enteros en orden de red |
| `node.cpp` | Ciclo de vida del nodo, asignación y liberación en el home, consultas |
| `network.cpp` | Listeners TCP, validación y despacho de mensajes, solicitudes pendientes, heartbeat, JOIN, hilo de servicio |
| `pager.cpp` | Región DSM (`memfd` + `mmap`), `mprotect`, manejador de `SIGSEGV`, hilo de fallos y directorio MSI |
| `dsm.cpp` | Implementación del API en C |
| `main.cpp` | Programa `dsmnode` |
| `apps/` | Aplicaciones `wordcount` y `matmul` |
| `test_memoria.cpp`, `test_coherencia.cpp` | Pruebas automáticas |

## API

| Función | Descripción |
|---|---|
| `dsm_init(config, node_id)` / `dsm_shutdown()` | Inician y cierran el nodo del proceso |
| `dsm_alloc(size, proc, &ptr)` | Reserva páginas contiguas en un home en línea (orden rotativo) |
| `dsm_free(ptr)` | Libera la asignación e invalida las copias remotas |
| `dsm_nodes(out, max)` | Tabla de nodos |
| `dsm_stats(node_id, out)` | Estadísticas de cualquier nodo en línea |
| `dsm_page_table(node_id, first, count, out)` | Tabla de páginas del home `node_id` (hasta 256 entradas) |
| `dsm_page_address(page)` | Dirección de una página global |
| `dsm_page_size()`, `dsm_strerror(err)` | Utilidades |

Los punteros que devuelve `dsm_alloc` se usan como memoria normal en cualquier nodo.

## Límites conocidos

- Un nodo que no responde un `INVALIDATE` se da por caído y sin copia. Si en realidad estaba
  lento, la coherencia de esa página se rompe.
- Si el home de una página se cae, sus datos se pierden; no hay replicación.
- Un acceso a una página no asignada, o cuyo home no responde, termina el proceso con
  `SIGSEGV`.
- La detección de lectura o escritura en el manejador usa el registro de error de x86_64.
- Una asignación no puede superar `pages_per_node` páginas.
