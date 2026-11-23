# DSM prototype overview

## Main types

| Type | Purpose |
|---|---|
| `Node` | Runs a DSM node: TCP listeners, heartbeat and JOIN loops, message validation/dispatch, pending requests, home allocation, and the public API's local allocation records. |
| `NodeTable` | Tracks configured nodes, liveness, page totals, free-page counts, and statistics. |
| `Config` | Runtime settings loaded from `dsm.conf`, including the DSM base, page size, per-node capacity, timeouts, ports, and hosts. |
| `MsgHeader` | Logical fields for a protocol frame. Header fields are encoded and decoded individually rather than sending the C++ struct directly. |
| `NodeInfo` | Node address, page range, ID, and online/offline state. |
| `PageEntry` | Per-page metadata: process, copyset, allocation order, MSI state, and owner. |
| `NodeStats` | Page counts and DSM activity counters for a node. |
| `Pending` | Correlates a request with its expected peer and reply type; carries the response and wakes the waiting thread. |
| `Allocation` | Records an allocated page range and its owning process. Used by the home and by the allocating node. |
| `AllocReq`, `QueryReq` | Logical payload shapes for allocation and query requests; wire payloads are serialized field by field. |
| `MsiAccessPolicy` | Describes MSI access decisions. Local faults already use it; page transfer and invalidation remain future work. |

## Enums and helpers

`Channel` distinguishes data, control, and heartbeat traffic. `MessageType` lists protocol operations, while `NodeState`, `PageState`, and `PageAccess` describe node liveness and page-access state. Header/payload encoding helpers implement network byte order; configuration and address helpers parse settings, resolve hosts, and calculate per-node ports.

## C API

`dsm_init` / `dsm_shutdown` manage the process's node. `dsm_alloc` / `dsm_free` reserve and release page-rounded allocations. `dsm_nodes`, `dsm_stats`, and `dsm_page_table` expose node metadata and page information; `dsm_page_size` and `dsm_strerror` provide utility information. Remote stats and page-table queries are not implemented yet.

## Work status and remaining plan

| Day | Status | Code blocks and incremental checks |
|---|---|---|
| **1 — Control protocol and allocation** | **Done** | Field-by-field serialization and validation; `JOIN` / `JOIN_ACK` with sequence-correlated pending requests and timeout; contiguous home allocation plus allocation/free requests and C API. Tested with two local node processes, including local and remote allocation, freeing, and free-page accounting. |
| **2 — Local DSM memory and faults** | **Done** | `setupMemory` maps one `memfd` twice: a `PROT_NONE` view fixed at `dsm_base` for applications and a private RW alias for the node. `protectPage` maps `I`/`S`/`M` to `PROT_NONE`/`PROT_READ`/`PROT_READ\|PROT_WRITE`. The `SIGSEGV` handler (`sigaction` + `SA_SIGINFO`) writes `{page, write}` to a pipe and waits on a per-page atomic counter; `faultLoop` resolves one fault at a time with the MSI policy and updates stats. Freed pages go back to `I` and are zeroed. Only pages homed on the local node resolve; remote pages need Day 3. Tested by `test_memoria.cpp`. |
| **3 — Page coherence and remote queries** | Remaining | **1. Page requests/data:** implement `PAGE_REQUEST` / `PAGE_DATA`; test full-page transfer between two nodes. **2. Invalidations/ACKs:** implement `INVALIDATE` / `INVALIDATE_ACK` before granting write ownership; test shared reads followed by exclusive writes. **3. Remote queries and integration:** implement `QUERY_REQUEST` / `QUERY_REPLY`, connect faults to transfers and MSI transitions, and test `alloc → remote access → invalidation → query → free`, including timeout/node-down cases. |

Allocation return values can be read and written when the home is the local node. Accessing a page homed on another node ends the process until `PAGE_REQUEST` exists.

Build and test (Linux):

```
g++ -std=c++17 -pthread prototipo_2.cpp -o dsmnode
g++ -std=c++17 -pthread -DDSM_NO_MAIN test_memoria.cpp prototipo_2.cpp -o test_memoria && ./test_memoria
```
