# dm_simulator

A single-machine discrete-event simulator for a simplified disaggregated
memory system.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
