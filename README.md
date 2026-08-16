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
Start Yggdrasil with a database file:
```bash
./build/yggdrasil-cli test.db
```

The interactive CLI will be available:
```bash
Yggdrasil database opened: test.db
Type 'help' for available commands.
yggdrasil> put name dong
OK
yggdrasil> get name
dong
yggdrasil> put age 20
OK
yggdrasil> scan
age = 20
name = dong
yggdrasil> stats
Pages:              2
Buffer pool:        1024
Resident frames:    1
Buffer pool hit:    100%
Tree height:        1
yggdrasil> verify
OK
yggdrasil> exit
```
---

# Benchmark
