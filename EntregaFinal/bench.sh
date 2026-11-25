#!/bin/sh
# Mide las tres aplicaciones con 1, 2 y 3 nodos en esta máquina (un proceso por nodo).
# Uso: make bench   (o ./bench.sh después de make all data/texto_grande.txt)
# Cada nodo aporta 1024 páginas de 4 KB (4 MB).

run() {
    nodes=$1
    app=$2
    shift 2
    conf=/tmp/dsm_bench_$nodes.conf
    {
        echo "page_size = 4096"
        echo "pages_per_node = 1024"
        echo "heartbeat_ms = 500"
        echo "timeout_ms = 5000"
        echo "port_base = $((28600 + nodes * 20))"
        echo "dsm_base = 0x100000000000"
        i=0
        while [ "$i" -lt "$nodes" ]; do
            echo "host = 127.0.0.1"
            i=$((i + 1))
        done
    } > "$conf"
    i=1
    while [ "$i" -lt "$nodes" ]; do
        "./$app" "$conf" "$i" "$@" > "/tmp/dsm_bench_$i.out" 2>&1 &
        i=$((i + 1))
    done
    "./$app" "$conf" 0 "$@" > /tmp/dsm_bench_0.out 2>&1
    wait
    echo "== $app con $nodes nodo(s)"
    cat /tmp/dsm_bench_*.out | grep -E "tiempos|verificación|fallos de página|necesita|Out of memory"
    rm -f /tmp/dsm_bench_*.out "$conf"
}

for nodes in 1 2 3; do
    run "$nodes" matmul data/matriz_a.txt data/matriz_b.txt
    run "$nodes" wordcount data/texto_grande.txt
    run "$nodes" imgfilter data/aldrin.ppm
done
rm -f data/*_lapiz_n*.pgm
