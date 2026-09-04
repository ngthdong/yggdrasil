# Yggdrasil

Yggdrasil is a key-value storage engine built from scratch to explore the internals of modern database systems. It provides persistent page-oriented storage, buffer pool management, and a B+Tree index with point lookups, updates, deletion, and ordered range scans through a simple database API.

---

## Features 

---

## Architecture

---

## Getting Started

### Installation

Clone the repository:

```bash
git clone https://github.com/ngthdong/yggdrasil.git
cd yggdrasil
```

Build the project:
```
cmake -S . -B build
cmake --build build -j
```

### Running the CLI
Start Yggdrasil with a database file (created automatically if it doesn't exist):
```bash
./build/yggdrasil-cli test.db
```

Options the engine was opened with can be set via startup flags:

| Flag | Description | Default |
|---|---|---|
| `--page-size N` | Page size in bytes, power of two ≥ 512 | `4096` |
| `--buffer-pool-frames N` | Buffer pool capacity, in frames | `1024` |
| `--no-create` | Fail instead of creating a missing database file | off |
| `--no-sync` | Don't fsync the WAL after every commit | off |
| `--deadlock-detection` | Use background deadlock detection instead of wound-wait | off |
| `--detection-interval-ms N` | Detection scan interval, in ms | `50` |

```bash
./build/yggdrasil-cli --page-size 8192 --deadlock-detection test.db
```

### Commands

At the `yggdrasil>` prompt, `help` lists all commands. They fall into five groups:

**Key-value**
```
put <key> <value>            Insert or update a key
get <key>                    Get a value
delete <key>                 Delete a key (alias: remove)
scan [--start K] [--end K]   Scan keys in sorted order, optionally bounded
```

**Transactions** — `put`/`get`/`delete` transparently operate on the active transaction while one is open:
```
begin                        Start a transaction
commit                       Commit the active transaction
rollback                     Roll back the active transaction
```

**Atomic write batches** — accumulate several operations and apply them as one atomic write:
```
batch begin                  Start accumulating a batch
batch put <key> <value>      Add a Put to the pending batch
batch delete <key>           Add a Delete to the pending batch
batch commit                 Apply the pending batch atomically
batch cancel                 Discard the pending batch
batch status                 Show how many ops are pending
```

**Snapshots** — read-only, point-in-time views that stay unaffected by later writes:
```
snapshot create               Create a snapshot, printing its id
snapshot get <id> <key>       Read a key as of that snapshot
snapshot list                 List open snapshot ids
snapshot close <id>           Close a snapshot and free its backing file
```

**Maintenance and inspection**
```
stats                         Show database statistics (pages, buffer pool, LSNs, tree height)
verify [--deep]               Verify database invariants; --deep also cross-checks scan order/values
checkpoint                    Flush all dirty pages and record a checkpoint
info                          Show the options this database was opened with
status                        Show CLI session state (active transaction/batch/open snapshots)
help                          Show the command list
exit                          Exit the CLI (rolls back any open transaction, discards any pending batch)
```

### Example session

```bash
Yggdrasil database opened: test.db
Type 'help' for available commands.
yggdrasil> put name dong
OK
yggdrasil> put age 20
OK
yggdrasil> scan
age = 20
name = dong
2 key(s)
yggdrasil> begin
OK (transaction started)
yggdrasil> put age 21
OK
yggdrasil> rollback
OK
yggdrasil> get age
20
yggdrasil> snapshot create
OK (snapshot #1)
yggdrasil> put age 21
OK
yggdrasil> snapshot get 1 age
20
yggdrasil> get age
21
yggdrasil> batch begin
OK (batch started)
yggdrasil> batch put city hcmc
OK (1 op(s) pending)
yggdrasil> batch commit
OK (1 op(s) applied)
yggdrasil> checkpoint
OK
yggdrasil> stats
Pages:              2
Buffer pool:        1024
Resident frames:    1
Buffer pool hit:    100%
Tree height:        1
Durable LSN:        12
Last checkpoint:    11
yggdrasil> verify --deep
OK (deep: 3 key(s) cross-checked)
yggdrasil> exit
```
---

## Benchmark
