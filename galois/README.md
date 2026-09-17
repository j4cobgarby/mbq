# Galois
This is a minimalistic version of the original Galois repo.
It only contains necessary files for running SSSP, PPSP, BFS, and PR.

The MultiQueue-based implementations are in `lonestar/analytics/<benchmark>/<benchmark>.cpp` file, 
along with the OBIM/PMOD-based implementaions.

The MultiQueue-based implementations rely on the MultiQueue files from the cps repo.

# Prerequisites
The executables are built with CMake 3.27.7, GCC 12, libllvm 14 with RTTI support, libfmt >= 4.0, and boost >= 1.58.0.



# Build & Run
```
# build
./build.sh

# run sssp with OBIM/PMOD
./runssspgalois.sh

# run sssp with MQs
./runssspmq.sh

# other benchmarks are ran just like sssp
# with corresponding scripts
./run<sssp/ppsp/bfs/pr><galois/mq>.sh
```
