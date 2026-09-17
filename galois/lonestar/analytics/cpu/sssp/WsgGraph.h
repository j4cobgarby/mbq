#ifndef LONESTAR_SSSP_WSGGRAPH_H
#define LONESTAR_SSSP_WSGGRAPH_H

// Reads the GAP Benchmark Suite serialized weighted graph format (.wsg), so
// that this driver consumes the very same graph file as the other
// implementations in the relax-experiments harness. Node numbering is then
// identical by construction, which is what makes a shared .sources file valid.
//
// Layout, as written by gapbs / wasp's include/writer.h:
//
//   bool    directed
//   int64   num_edges                 (directed edge count)
//   int64   num_nodes
//   int64   offsets[num_nodes + 1]    out-edge CSR offsets
//   struct { int32 dst; W weight; }   [num_edges]
//   if directed:
//     int64 in_offsets[num_nodes + 1]
//     struct { int32 src; W weight; } [num_edges]   (unused by SSSP)
//
// The file is mapped rather than read into a buffer: these graphs reach
// billions of edges, and the edge section is consumed once, in order.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "galois/Galois.h"

namespace wsg {

//! Mapped read-only view of a file, unmapped on destruction.
class MappedFile {
public:
  explicit MappedFile(const std::string& filename) {
    fd_ = open(filename.c_str(), O_RDONLY);
    if (fd_ == -1) {
      std::cerr << "Could not open graph " << filename << "\n";
      abort();
    }
    // lseek rather than fstat: <sys/stat.h> would drag `struct stat` into the
    // including translation unit, and at least one driver defines its own.
    off_t end = lseek(fd_, 0, SEEK_END);
    if (end == -1) {
      std::cerr << "Could not size graph " << filename << "\n";
      abort();
    }
    length_ = static_cast<size_t>(end);
    data_   = static_cast<const char*>(
        mmap(nullptr, length_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (data_ == MAP_FAILED) {
      std::cerr << "Could not map graph " << filename << "\n";
      abort();
    }
  }

  ~MappedFile() {
    if (data_ != MAP_FAILED)
      munmap(const_cast<char*>(data_), length_);
    if (fd_ != -1)
      close(fd_);
  }

  MappedFile(const MappedFile&)            = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const char* data() const { return data_; }
  size_t size() const { return length_; }

private:
  int fd_          = -1;
  size_t length_   = 0;
  const char* data_ = static_cast<const char*>(MAP_FAILED);
};

//! One out-edge as stored in the file. WeightTy must match the type the graph
//! was written with, which is why the binaries are built per weight type.
template <typename WeightTy>
struct WsgEdge {
  int32_t dst;
  WeightTy weight;
};

} // namespace wsg

//! Builds `graph` from the .wsg file at `filename`.
template <typename Graph, typename WeightTy>
void readWsgGraph(Graph& graph, const std::string& filename) {
  using Edge = wsg::WsgEdge<WeightTy>;

  wsg::MappedFile file(filename);
  const char* p = file.data();

  bool directed;
  std::memcpy(&directed, p, sizeof(bool));
  p += sizeof(bool);

  int64_t numEdges, numNodes;
  std::memcpy(&numEdges, p, sizeof(int64_t));
  p += sizeof(int64_t);
  std::memcpy(&numNodes, p, sizeof(int64_t));
  p += sizeof(int64_t);

  if (numNodes < 0 || numEdges < 0) {
    std::cerr << "Corrupt .wsg header in " << filename << "\n";
    abort();
  }

  const int64_t* offsets = reinterpret_cast<const int64_t*>(p);
  p += static_cast<size_t>(numNodes + 1) * sizeof(int64_t);
  const Edge* edges = reinterpret_cast<const Edge*>(p);

  // The whole file size is derived from the header and required to match
  // exactly. A weaker "does the out-edge section fit" check is not enough: a
  // directed graph carries a second CSR of the same size, so a binary reading
  // with a too-wide weight type still fits inside the file and then silently
  // misparses every edge.
  size_t csrBytes = static_cast<size_t>(numNodes + 1) * sizeof(int64_t) +
                    static_cast<size_t>(numEdges) * sizeof(Edge);
  size_t expected =
      sizeof(bool) + 2 * sizeof(int64_t) + (directed ? 2 * csrBytes : csrBytes);
  if (file.size() != expected) {
    std::cerr << "Graph " << filename << " is " << file.size()
              << " bytes but its header implies " << expected << " (" << numNodes
              << " nodes, " << numEdges << " edges, " << sizeof(Edge)
              << "-byte edges). This binary expects a " << sizeof(WeightTy)
              << "-byte weight type; the file was probably written with a "
              << "different one.\n";
    abort();
  }

  std::cout << "Read " << numNodes << " nodes, " << numEdges << " edges from "
            << filename << (directed ? " (directed)" : " (undirected)") << "\n";

  // Built through FileGraphWriter, the public in-memory construction path,
  // rather than by filling the LC_CSR arrays directly. A hand-rolled fill also
  // has to register each thread's node slice via setLocalRange, which is not
  // publicly reachable; skip it and galois::iterate(graph) walks no nodes at
  // all, so every parallel loop silently does nothing while direct getData
  // calls keep working and hide the breakage.
  galois::graphs::FileGraphWriter writer;
  writer.setNumNodes(static_cast<size_t>(numNodes));
  writer.setNumEdges<WeightTy>(static_cast<size_t>(numEdges));

  writer.phase1();
  for (int64_t n = 0; n < numNodes; ++n)
    writer.incrementDegree(static_cast<size_t>(n),
                           static_cast<uint64_t>(offsets[n + 1] - offsets[n]));

  // addNeighbor returns the slot each edge landed in, but the edge data array
  // only exists after finish(), which also frees the scratch addNeighbor needs.
  // So stage the weights by slot first, then copy them across.
  writer.phase2();
  std::vector<WeightTy> staged(static_cast<size_t>(numEdges));
  for (int64_t n = 0; n < numNodes; ++n) {
    for (int64_t e = offsets[n]; e < offsets[n + 1]; ++e) {
      size_t idx   = writer.addNeighbor(static_cast<size_t>(n),
                                        static_cast<size_t>(edges[e].dst));
      staged[idx] = static_cast<WeightTy>(edges[e].weight);
    }
  }

  auto* edgeData = writer.finish<WeightTy>();
  std::uninitialized_copy(staged.begin(), staged.end(), edgeData);
  std::vector<WeightTy>().swap(staged);

  galois::graphs::readGraph(graph, writer);
}

//! True when the harness handed us a GAP serialized graph rather than a .gr.
inline bool isWsgFilename(const std::string& filename) {
  return filename.size() >= 4 &&
         filename.compare(filename.size() - 4, 4, ".wsg") == 0;
}

#endif
