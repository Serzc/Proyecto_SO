#ifndef DSM_H
#define DSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dsm_err {
    DSM_OK = 0,
    DSM_E_INVAL = -1,
    DSM_E_CONFIG = -2,
    DSM_E_NOMEM = -3,
    DSM_E_BADADDR = -4,
    DSM_E_NODE_DOWN = -5,
    DSM_E_TIMEOUT = -6,
    DSM_E_NET = -7,
    /* The operation depends on DSM page transfer/allocation/query support. */
    DSM_E_NOT_IMPLEMENTED = -8
} dsm_err_t;

typedef struct dsm_node {
    uint32_t ipv4;
    uint32_t first_page;
    uint32_t num_pages;
    uint8_t node_id;
    uint8_t state;
    uint16_t reserved;
} dsm_node_t;

typedef struct dsm_page {
    uint32_t proc_id;
    uint32_t copyset;
    uint32_t order;
    uint8_t state;
    uint8_t owner;
    uint16_t reserved;
} dsm_page_t;

typedef struct dsm_stats {
    uint32_t total_pages;
    uint32_t free_pages;
    uint32_t pages_m;
    uint32_t pages_s;
    uint32_t pages_i;
    uint32_t faults;
    uint32_t pages_sent;
    uint32_t pages_recv;
} dsm_stats_t;

int dsm_init(const char* config_path, int node_id);
void dsm_shutdown(void);
/* These return DSM_E_NOT_IMPLEMENTED until the DSM memory/protocol is implemented. */
int dsm_alloc(size_t size, uint16_t proc, void** ptr);
int dsm_free(void* ptr);
int dsm_nodes(dsm_node_t* out, int max);
int dsm_stats(int node_id, dsm_stats_t* out);
int dsm_page_table(int node_id, uint32_t first_page, uint32_t count,
                   dsm_page_t* out);
size_t dsm_page_size(void);
const char* dsm_strerror(int err);

/* Define DSM_NO_MAIN when compiling prototipo.cpp as a library object. */
#ifdef __cplusplus
}
#endif

#endif
