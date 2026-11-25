# Sistema DSM: biblioteca, nodo de consola, aplicaciones de prueba y pruebas.
CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
LDFLAGS ?= -pthread

LIB_OBJS = config.o protocol.o node.o network.o pager.o dsm.o
PROGRAMS = dsmnode wordcount matmul test_memoria test_coherencia

all: $(PROGRAMS)

libdsm.a: $(LIB_OBJS)
	ar rcs $@ $^

%.o: %.cpp dsm_internal.h dsm.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

apps/%.o: apps/%.c apps/app_common.h dsm.h
	$(CC) $(CFLAGS) -c $< -o $@

# Las mismas aplicaciones sin main, enlazadas en dsmnode para el comando run.
apps/%_lib.o: apps/%.c apps/app_common.h dsm.h
	$(CC) $(CFLAGS) -DAPP_NO_MAIN -c $< -o $@

dsmnode: main.o apps/wordcount_lib.o apps/matmul_lib.o libdsm.a
	$(CXX) $(LDFLAGS) $^ -o $@

wordcount: apps/wordcount.o libdsm.a
	$(CXX) $(LDFLAGS) $^ -o $@

matmul: apps/matmul.o libdsm.a
	$(CXX) $(LDFLAGS) $^ -o $@

test_memoria: test_memoria.o libdsm.a
	$(CXX) $(LDFLAGS) $^ -o $@

test_coherencia: test_coherencia.o libdsm.a
	$(CXX) $(LDFLAGS) $^ -o $@

# Pruebas automáticas en una sola máquina (puertos 28500-28525).
test: test_memoria test_coherencia
	./test_memoria
	./test_coherencia 0 & pid=$$!; sleep 1; ./test_coherencia 1 && wait $$pid

clean:
	rm -f *.o apps/*.o libdsm.a $(PROGRAMS)

.PHONY: all test clean
