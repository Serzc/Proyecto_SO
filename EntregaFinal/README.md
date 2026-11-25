# Sistema DSM (IC-6600, II Semestre 2026)

Memoria compartida distribuida en espacio de usuario para Linux, con paginación bajo demanda y coherencia MSI.
Autores: Sergio Andrés Zúñiga Castillo y José Ignacio Madriz Ortiz.

## Requisitos

- Linux x86_64, kernel 4.17 o posterior.
- `g++` (C++17), `gcc` y `make`.
- Los mismos puertos TCP abiertos entre todas las máquinas.

## Compilar y probar

```
make          # compila libdsm.a, dsmnode, wordcount, matmul y las pruebas
make test     # pruebas automáticas en una sola máquina
```

## Configuración (`dsm.conf`)

Todas las máquinas usan el mismo archivo. El orden de las líneas `host` define el `node_id`.

| Opción | Por defecto | Uso |
|---|---|---|
| `page_size` | 4096 | Tamaño de página |
| `pages_per_node` | 256 | Páginas que aporta cada nodo |
| `heartbeat_ms` | 1000 | Periodo del heartbeat |
| `timeout_ms` | 5000 | Tiempo para dar un nodo por caído o una solicitud por perdida |
| `port_base` | 28400 | El nodo k usa los puertos `port_base + 3k` a `port_base + 3k + 2` |
| `dsm_base` | 0x100000000000 | Dirección donde empieza el espacio DSM |
| `host` | | IPv4 de cada nodo, una línea por nodo |

Para varios nodos en una sola máquina se repite `127.0.0.1` (ejemplo: `apps/local3.conf`).

## Ejecutar

En cada máquina se inicia un nodo con su consola:

```
./dsmnode dsm.conf <id>
```

Comandos: `nodes`, `stats`, `pages <id>`, `run <app> [args]`, `monitor`, `help` y `quit`. Los mensajes recibidos y los cambios de estado de los nodos no interrumpen la consola: se ven con `monitor` (Enter para salir). `pages` abre una vista por nodo (flechas izquierda y derecha para cambiar de nodo, `q` para salir) con la memoria usada y libre, un mapa de páginas por proceso y las asignaciones con sus copias. Las aplicaciones corren dentro del nodo ya iniciado:

```
run matmul                                       # en los nodos 1, 2, ... (trabajadores)
run matmul data/matriz_a.txt data/matriz_b.txt   # en el nodo 0 (coordinador)
run wordcount                                    # trabajadores
run wordcount data/enunciado_dsm.txt             # coordinador
```

El coordinador compara el resultado con un cálculo secuencial e imprime `CORRECTA` o `ERROR`. `quit` espera a que terminen las aplicaciones en curso.

Las aplicaciones también existen como programas aparte que inician su propio nodo: `./matmul dsm.conf <id> [archivos]` y `./wordcount dsm.conf <id> [archivo]`.

## Arquitectura

Cada proceso que llama a `dsm_init` es un nodo par. Todos los nodos mapean el mismo rango de direcciones con `mmap`, con las páginas en `PROT_NONE`. Al tocar una página sin permiso ocurre un `SIGSEGV`. El manejador pasa la página a un hilo de fallos, que la pide por TCP a su nodo home y aplica `mprotect` según el estado MSI (I sin acceso, S lectura, M lectura y escritura). El home guarda un directorio con el dueño y las copias de cada página, invalida las copias antes de conceder escritura y recupera las páginas modificadas.

| Módulo | Contenido |
|---|---|
| `pager.cpp` | Memoria, `SIGSEGV`, fallos y directorio MSI |
| `network.cpp` | TCP, protocolo de mensajes, heartbeat |
| `node.cpp` | Ciclo de vida del nodo y asignación |
| `config.cpp`, `protocol.cpp` | Configuración y codificación de mensajes |
| `dsm.cpp`, `dsm.h` | API en C |
| `main.cpp` | Consola `dsmnode` |

El informe técnico (`SO.2026 - Proyecto Entrega Final - Informe tecnico.pdf`) explica el diseño completo.
