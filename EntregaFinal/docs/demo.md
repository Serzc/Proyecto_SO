# Guion de la demostración

Tres computadoras del laboratorio (A, B y C), un nodo en cada una. Duración estimada: 30 a 40 minutos.

## Antes de la cita

1. En cada máquina: `git clone https://github.com/Serzc/Proyecto_SO`, `cd Proyecto_SO/EntregaFinal` y `make`.
2. Anotar la IPv4 de cada máquina (`ip -4 addr`) y escribir `dsm.conf` igual en las tres, con el `port_base` del rango asignado:

   ```
   page_size = 4096
   pages_per_node = 1024
   heartbeat_ms = 1000
   timeout_ms = 5000
   port_base = 28400
   host = <IP de A>
   host = <IP de B>
   host = <IP de C>
   ```

3. Comprobar que los puertos `port_base` a `port_base + 8` no estén bloqueados por el firewall.
4. `make test` en una máquina, para confirmar que compila y pasa.
5. Los archivos de prueba (`data/`) solo hacen falta en la máquina A.

## 1. Arranque y heartbeat (5 min)

1. En A: `./dsmnode dsm.conf 0`. En B: `./dsmnode dsm.conf 1`. En C: `./dsmnode dsm.conf 2`.
2. En A: `nodes`. Los tres nodos aparecen EN LINEA.
3. En A: `monitor`. Se ven los JOIN y los cambios de estado. Enter para salir.

Qué explicar: los nodos son pares, no hay servidor; el JOIN lleva la configuración y se valida; tres puertos por nodo; heartbeat y timeout.

## 2. Ejecución distribuida (10 min)

1. En A: `run matmul data/matriz_a.txt data/matriz_b.txt`. B y C no escriben nada.
2. Mostrar en B y C las líneas "filas [...] de C calculadas con 4 pthreads" y "verificación del nodo 0: CORRECTA".
3. En A: `run imgfilter data/aldrin.ppm`. Mientras corre, en B: `pages all`.
4. Abrir `output/aldrin_lapiz.pgm` en A.
5. En A: `run wordcount data/enunciado_dsm.txt`.

Qué explicar: el origen lee los archivos y los demás reciben la entrada por el DSM; segmentos compartidos por número de proceso; la imagen no cabe en un nodo y se reparte; barreras sobre páginas de control.

## 3. Páginas y coherencia (10 min)

1. En cualquier nodo: `stats`. Páginas libres, copias M/S/I, fallos y transferencias por nodo.
2. `pages`, con las flechas para pasar de nodo en nodo: mapa de páginas por proceso, asignaciones y log de solicitudes (quién pidió qué página, qué se concedió, a quién se invalidó y de dónde vino el write-back).
3. `pages 1` y `pages all`: proceso, orden, dueño, estado y copias de cada página.

Qué explicar: `SIGSEGV`, `si_addr`, el pipe hacia el hilo de fallos y `mprotect`; estados M, S e I; `INVALIDATE` con write-back; semáforos POSIX en el directorio y en las solicitudes pendientes; por qué no hay interbloqueos.

## 4. Fallas (5 min)

1. Cerrar el nodo C con `quit` (o Ctrl+C). En A, `monitor` muestra "Nodo 2: CAIDO" después de `timeout_ms`.
2. Volver a iniciar C con `./dsmnode dsm.conf 2`; vuelve a EN LINEA en A. No lanzar `run` mientras un nodo está caído: las aplicaciones esperan a todos los nodos de `dsm.conf`.
3. Configuración distinta: en C, copiar `dsm.conf` a `otro.conf`, cambiar `pages_per_node` y ejecutar `./dsmnode otro.conf 2`. En A, `monitor` muestra "El nodo 2 tiene un dsm.conf distinto". Cerrar y reiniciar C con `dsm.conf`.

Qué explicar: qué pasa si cae un home (los datos se pierden, no hay replicación) y cuáles son las limitaciones.

## Si algo falla

- Un nodo no pasa a EN LINEA: revisar las IP de `dsm.conf`, el firewall y que todos usen el mismo archivo (`monitor` avisa si no).
- `run` se queda esperando: algún nodo de `dsm.conf` no está iniciado.
- `imgfilter` dice que no cabe: `pages_per_node` debe ser 1024.
