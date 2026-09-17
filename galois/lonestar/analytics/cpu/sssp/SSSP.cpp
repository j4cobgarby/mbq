/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/Galois.h"
#include "galois/AtomicHelpers.h"
#include "galois/Reduction.h"
#include "galois/PriorityQueue.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "Lonestar/BoilerPlate.h"
#include "Lonestar/BFS_SSSP.h"
#include "Lonestar/Utils.h"
#include "WsgGraph.h"

#include <include/MultiBucketQueue.h>
#include <include/MultiQueue.h>

#include "llvm/Support/CommandLine.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <type_traits>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

// #define PERF 1

namespace cll = llvm::cl;

static const char* name = "Single Source Shortest Path";
static const char* desc =
    "Computes the shortest path from a source node to all nodes in a directed "
    "graph using a modified chaotic iteration algorithm";
static const char* url = "single_source_shortest_path";

static cll::opt<std::string>
    inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<unsigned int>
    startNode("startNode",
              cll::desc("Node to start search from (default value 0)"),
              cll::init(0));
static cll::opt<unsigned int>
    reportNode("reportNode",
               cll::desc("Node to report distance to(default value 1)"),
               cll::init(1));
static cll::opt<unsigned int>
    stepShift("delta",
              cll::desc("Shift value for the deltastep (default value 13)"),
              cll::init(13));
// Worker threads for the MQ/MQBucket schedulers, which spawn their own pthreads
// rather than using Galois's pool. Upstream called this -threads, but current
// LLVM registers a `threads` option of its own inside libLLVM and two
// registrations of one name abort at startup, hence the rename.
//
// Deliberately NOT Galois's -t: LonestarStart pushes that through
// setActiveThreads(), which silently clamps to getMaxUsableThreads() -- the
// process's CPU affinity intersected with /proc/cpuinfo. Where that detection
// comes back small, driving the MQ from it runs the scheduler on one thread
// while still reporting the requested count everywhere else.
static cll::opt<unsigned int>
    mqThreads("mqthreads",
              cll::desc("Worker threads for the MQ/MQBucket schedulers "
                        "(default: follow -t)"),
              cll::init(0));
static cll::opt<unsigned int>
    queueNum("queues",
              cll::desc("number of queues for MQ"),
              cll::init(4));
static cll::opt<unsigned int>
    bucketNum("buckets",
              cll::desc("number of buckets in a bucket queue"),
              cll::init(64));
static cll::opt<unsigned int>
    batch1("batch1",
              cll::desc("batch size for popping"),
              cll::init(1));
static cll::opt<unsigned int>
    batch2("batch2",
              cll::desc("batch size for pushing"),
              cll::init(1));
static cll::opt<unsigned int>
    chunk("chunk",
              cll::desc("chunk size for tuning"),
              cll::init(64));
static cll::opt<unsigned int>
    stickiness("stick",
              cll::desc("stickiness"),
              cll::init(1));
static cll::opt<unsigned int>
    prefetch("prefetch",
              cll::desc("prefetching"),
              cll::init(0));
// Added for the relax-experiments harness: run several sources per invocation,
// taken from the same .sources file the other implementations read, so every
// implementation is measured on identical source vertices.
static cll::opt<std::string>
    sourcesFile("sfile",
              cll::desc("File of source nodes, one per line; overrides startNode"),
              cll::init(""));
static cll::opt<unsigned int>
    numSources("sources",
              cll::desc("Number of sources to run from sfile"),
              cll::init(1));
static cll::opt<unsigned int>
    numRounds("rounds",
              cll::desc("Number of trials per source"),
              cll::init(1));
// Opt-in rather than reusing -noverify: llvm::cl rejects a repeated option, so
// the harness cannot pass a default and then override it.
static cll::opt<bool>
    verifyEach("verifyeach",
              cll::desc("Verify the result after each source"),
              cll::init(false));

enum Algo {
  deltaStepOBIM,
  deltaStepPMOD,
  MQ,
  MQBucket,
  serial,
};

const char* const ALGO_NAMES[] = {
    "deltaStepOBIM", "deltaStepPMOD", "MQ", "MQBucket", "serial"};
static cll::opt<Algo> algo(
    "algo", cll::desc("Choose an algorithm (default value auto):"),
    cll::values(clEnumVal(deltaStepOBIM, "deltaStepOBIM"),
                clEnumVal(deltaStepPMOD, "deltaStepPMOD"),
                clEnumVal(MQ, "MQ"),
                clEnumVal(MQBucket, "MQBucket"),
                clEnumVal(serial, "serial")));

// Weight type as stored in the graph file, and the type distances are computed
// in. Built both ways so the harness can address <binary>-int32 / <binary>-float,
// matching how the graph files themselves are typed.
#ifdef USE_FLOAT
typedef float weight_type;
#else
typedef uint32_t weight_type;
#endif

//! [withnumaalloc]
using Graph =
    galois::graphs::LC_CSR_Graph<std::atomic<weight_type>, weight_type>::
        with_no_lockable<true>::type ::with_numa_alloc<true>::type;
//! [withnumaalloc]
typedef Graph::GraphNode GNode;

// "No distance yet" marker in the prios array. Was a literal UINT32_MAX, which
// is only the right sentinel while weights are 32-bit unsigned.
constexpr static const weight_type PRIO_UNREACHED =
    std::numeric_limits<weight_type>::max();

constexpr static const bool TRACK_WORK          = true;
constexpr static const unsigned CHUNK_SIZE      = 256U;
constexpr static const ptrdiff_t EDGE_TILE_SIZE = 512;

using SSSP                 = BFS_SSSP<Graph, weight_type, true, EDGE_TILE_SIZE>;
using Dist                 = SSSP::Dist;
using UpdateRequest        = SSSP::UpdateRequest;
using UpdateRequestIndexer = SSSP::UpdateRequestIndexer;
using SrcEdgeTile          = SSSP::SrcEdgeTile;
using SrcEdgeTileMaker     = SSSP::SrcEdgeTileMaker;
using SrcEdgeTilePushWrap  = SSSP::SrcEdgeTilePushWrap;
using ReqPushWrap          = SSSP::ReqPushWrap;
using OutEdgeRangeFn       = SSSP::OutEdgeRangeFn;
using TileRangeFn          = SSSP::TileRangeFn;

template <typename T, typename OBIMTy, typename P, typename R>
void deltaStepAlgo(Graph& graph, GNode source, const P& pushWrap,
                   const R& edgeRange) {

  //! [reducible for self-defined stats]
  galois::GAccumulator<size_t> BadWork;
  //! [reducible for self-defined stats]
  galois::GAccumulator<size_t> WLEmptyWork;

  graph.getData(source) = 0;
  galois::InsertBag<T> initBag;
  pushWrap(initBag, source, 0, "parallel");

  std::cout << "delta = " << stepShift << "\n";

  auto begin = std::chrono::high_resolution_clock::now();

  galois::for_each(
      galois::iterate(initBag), // range maker
			// function to run
      [&](const T& item, auto& ctx) {
        constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        const auto& sdata                 = graph.getData(item.src, flag);

        if (sdata < item.dist) {
          if (TRACK_WORK)
            WLEmptyWork += 1;
          return;
        }

        for (auto ii : edgeRange(item)) {

          GNode dst          = graph.getEdgeDst(ii);
          auto& ddist        = graph.getData(dst, flag);
          Dist ew            = graph.getEdgeData(ii, flag);
          const Dist newDist = sdata + ew;
          Dist oldDist       = galois::atomicMin<weight_type>(ddist, newDist);
          if (newDist < oldDist) {
            if (TRACK_WORK) {
              //! [per-thread contribution of self-defined stats]
              if (oldDist != SSSP::DIST_INFINITY) {
                BadWork += 1;
              }
              //! [per-thread contribution of self-defined stats]
            }
            pushWrap(ctx, dst, newDist);
          }
        }
      },

			// arguments
      galois::wl<OBIMTy>(UpdateRequestIndexer{stepShift}), // OBIM worklist

			// other settings
      galois::disable_conflict_detection(), galois::loopname("SSSP"));

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  std::cout << "runtime_ms " << ms << "\n";

  if (TRACK_WORK) {
    //! [report self-defined stats]
    galois::runtime::reportStat_Single("SSSP", "BadWork", BadWork.reduce());
    //! [report self-defined stats]
    galois::runtime::reportStat_Single("SSSP", "WLEmptyWork",
                                       WLEmptyWork.reduce());
  }
}

using PQElement = std::tuple<weight_type, uint32_t>;
// Renamed from `stat`: reading .wsg pulls in <fcntl.h>, and POSIX's struct stat
// then collides with this one.
struct mq_stat {
  uint64_t iter = 0;
  uint64_t emptyWork = 0;
};

weight_type __attribute__ ((noinline)) getPrioData(std::atomic<weight_type> *p) {
  return p->load(std::memory_order_acquire);
}

#ifdef PERF
bool __attribute__ ((noinline)) changeMin(
#else
inline bool changeMin(
#endif
  std::atomic<weight_type> *prios, uint32_t dst, weight_type oldDist, weight_type newDist) {
    weight_type d = oldDist;
    bool swapped = false;
    do {
      if (d <= newDist) break;
      swapped = prios[dst].compare_exchange_weak(
          d, newDist,
          std::memory_order_acq_rel,
          std::memory_order_acquire);
    } while(!swapped);
    return swapped;
}

template<typename MQ>
void MQThreadTask(Graph& graph, MQ &wl, mq_stat *stats, std::atomic<weight_type> *prios) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint dist;
  GNode src;
  wl.initTID();

  while (true) {
    auto item = wl.pop();
    if (item) std::tie(dist, src) = item.get();
    else break;

#ifdef PERF
    weight_type srcD = getPrioData(&prios[src]);
#else
    weight_type srcD = prios[src].load(std::memory_order_acquire);
#endif
    ++iter;
    if (srcD < dist) {
      // This filters out moot tasks when the vertex
      // being popped is given a lower distance
      emptyWork++;
      continue;
    }

    // Iterate neighbors and see if their distances can be lowered
    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {
      GNode dst   = graph.getEdgeDst(e);
      const auto newDist = srcD + graph.getEdgeData(e);
#ifdef PERF
      weight_type oldDist = getPrioData(&prios[dst]);
#else
      weight_type oldDist = prios[dst].load(std::memory_order_relaxed);
#endif
      // Attempt to CAS the neighbor to a lower distance
      if (changeMin(prios, dst, oldDist, newDist)) {
        wl.push(newDist, dst);
      }
    }
  }
  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<typename MQ_Type>
void spawnTasks(MQ_Type& wl, Graph& graph, const GNode& source, int threadNum, std::atomic<weight_type> *prios) {
  // init with source
  wl.push(0, source);

  mq_stat stats[threadNum];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (int i = 1; i < threadNum; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = i;
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      MQThreadTask<MQ_Type>, std::ref(graph), 
      std::ref(wl), &stats[i], std::ref(prios));
    int rc = pthread_setaffinity_np(newThread->native_handle(),
                                    sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
    }
    workers.push_back(newThread);
  }
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  sched_setaffinity(0, sizeof(cpuset), &cpuset);
  MQThreadTask<MQ_Type>(graph, wl, &stats[0], prios);

  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  wl.stat();
  std::cout << "runtime_ms " << ms << "\n";
  // Same region as runtime_ms above, but in seconds and without the
  // millisecond truncation, for the experiment harness to parse.
  std::cout << "External Trial Time: " << std::fixed << std::setprecision(6)
            << std::chrono::duration<double>(end - begin).count() << std::endl;

  if (!wl.empty()) {
    std::cout << "not empty!\n";
  }

  for (uint64_t i = 0; i < graph.size(); i++) {
    // Read back in weight_type: going through uint64_t here truncated every
    // fractional distance once weights could be floating point.
    weight_type s = prios[i].load(std::memory_order_relaxed);
    if (s == PRIO_UNREACHED) continue;
    auto& ddata = graph.getData(i);
    ddata = s;
  }

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (int i = 0; i < threadNum; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  galois::runtime::reportStat_Single("SSSP-MQ", "Iterations", totalIter);
  galois::runtime::reportStat_Single("SSSP-MQ", "Emptywork", totalEmptyWork);
}

template<bool usePrefetch=true>
void MQAlgo(Graph& graph, const GNode& source, int threadNum, int queueNum) {
  std::cout << "threads = " << threadNum << "\n";
  std::cout << "queues = " << queueNum << "\n";
  std::cout << "batchSizePop = " << batch1 << "\n";
  std::cout << "batchSizePush = " << batch2 << "\n";
  std::cout << "stickiness = " << stickiness << "\n";
  std::cout << "prefetch " << usePrefetch << "\n";

  // The distance array that records the latest distances
  std::atomic<weight_type> *prios = new std::atomic<weight_type>[graph.size()];
  for (uint i = 0; i < graph.size(); i++) {
    prios[i].store(PRIO_UNREACHED, std::memory_order_relaxed);
  }
  prios[source] = 0;
  graph.getData(source) = 0;

  // Prefetcher lambda for reducing cache misses on load to a 
  // vertex's distance
  auto prefetcher = [&] (uint32_t v) -> void {
     // priority of this node
    __builtin_prefetch(&prios[v], 0, 3);

    // the first and last of edges
    graph.prefetchEdgeStart(v);
    graph.prefetchEdgeEnd(v);
  };

  if (algo == MQ) {
    using MQ_Type = mbq::MultiQueue<decltype(prefetcher), std::greater<PQElement>, weight_type, uint32_t, usePrefetch>;
    MQ_Type wl(prefetcher, queueNum, threadNum, batch1, batch2, stickiness);
    spawnTasks<MQ_Type>(wl, graph, source, threadNum, prios);

  } else {
    // MQBucket
    std::cout << "buckets = " << bucketNum << "\n";
    std::cout << "delta = " << stepShift << "\n";

    // Lambda for mapping a priority to a priority level
    auto getBucketID = [&] (uint32_t v) -> mbq::BucketID {
      weight_type d = prios[v].load(std::memory_order_acquire);
      // A shift cannot bucket a floating-point priority, so divide by 2^delta.
      // #ifdef rather than if constexpr: weight_type is not a template
      // parameter here, so a discarded branch would still be type-checked.
#ifdef USE_FLOAT
      return (mbq::BucketID)(d / std::pow(2.0f, (float)stepShift));
#else
      return (mbq::BucketID)(d >> stepShift);
#endif
    };
    using MQ_Bucket_Type = mbq::MultiBucketQueue<decltype(getBucketID), decltype(prefetcher), std::greater<mbq::BucketID>, weight_type, uint32_t, usePrefetch>;
    MQ_Bucket_Type wl(getBucketID, prefetcher, queueNum, threadNum, stepShift, bucketNum, batch1, batch2, mbq::increasing, stickiness);
    spawnTasks<MQ_Bucket_Type>(wl, graph, source, threadNum, prios);
  }

  // Freed because the harness calls this once per source per round; leaking it
  // would cost graph.size() * 4 bytes per trial.
  delete[] prios;
}

void serialAlgo(Graph& graph, const GNode& source) {
  graph.getData(source) = 0;

  using SerPQ = std::priority_queue<
    PQElement,
    std::vector<PQElement>,
    std::greater<PQElement>
  >;
  SerPQ wl;

  wl.push({0, source});

  std::cout << "starting\n";
  auto begin = std::chrono::high_resolution_clock::now();
  uint64_t iter = 0, emptyWork = 0;
  uint64_t pushes = 0, pops = 0;
  uint dist, srcDist;
  GNode src;

  while (!wl.empty()) {
    ++iter;
    PQElement item = wl.top();
    std::tie(dist, src) = item;
    wl.pop();
    pops++;

    srcDist = graph.getData(src);
    if (srcDist < dist) {
      emptyWork++;
      continue;
    }

    auto edgeRange = graph.edges(src, galois::MethodFlag::UNPROTECTED);
    for (auto e : edgeRange) {

      GNode dst   = graph.getEdgeDst(e);
      auto& ddata = graph.getData(dst);

      const auto newDist = dist + graph.getEdgeData(e);
      if (newDist < ddata) {
        ddata = newDist;
        wl.push({newDist, dst});
        pushes++;
      }
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  std::cout << ms << " ms\n";

  galois::runtime::reportStat_Single("SSSP-Serial", "Iterations", iter);
  galois::runtime::reportStat_Single("SSSP-Serial", "EmptyWork", emptyWork);
}

//! Worker-thread count for the MQ schedulers: -mqthreads if given, else -t.
static int mqWorkerThreads() {
  return mqThreads > 0 ? (int)mqThreads : (int)numThreads;
}

//! Reads the harness's .sources file: one node id per line, plain text.
//! Falls back to the single -startNode when no file is given.
static std::vector<unsigned int> readSourceIds(size_t graphSize) {
  std::vector<unsigned int> sources;

  if (sourcesFile.empty()) {
    sources.push_back(startNode);
    return sources;
  }

  std::ifstream in(sourcesFile.c_str());
  if (!in) {
    std::cerr << "Could not open sources file " << sourcesFile << "\n";
    abort();
  }

  unsigned int s;
  while (sources.size() < numSources && in >> s) {
    if (s >= graphSize) {
      std::cerr << "Source " << s << " out of range for a graph of " << graphSize
                << " nodes; the sources file does not match this graph\n";
      abort();
    }
    sources.push_back(s);
  }

  if (sources.size() < numSources) {
    std::cerr << "Sources file " << sourcesFile << " holds only " << sources.size()
              << " usable sources, " << numSources << " requested\n";
    abort();
  }
  return sources;
}

//! Printed after every trial so that a node-numbering mismatch between this
//! graph format and the one other implementations read shows up immediately
//! rather than as quietly different numbers.
static void reportReach(Graph& graph) {
  galois::GReduceMax<weight_type> maxDistance;
  galois::GAccumulator<uint64_t> visitedNode;
  maxDistance.reset();
  visitedNode.reset();

  galois::do_all(
      galois::iterate(graph),
      [&](uint64_t i) {
        weight_type myDistance = graph.getData(i);

        if (myDistance != SSSP::DIST_INFINITY) {
          maxDistance.update(myDistance);
          visitedNode += 1;
        }
      },
      galois::loopname("Reach check"), galois::no_stats());

  std::cout << "Reached " << visitedNode.reduce() << " nodes, max dist "
            << std::setprecision(6) << maxDistance.reduce() << std::endl;
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph graph;
  GNode source;
  GNode report;

  // The harness hands us GAP serialized graphs (.wsg) so that every
  // implementation reads the identical file; .gr is still accepted.
  if (isWsgFilename(inputFile)) {
    readWsgGraph<Graph, weight_type>(graph, inputFile);
  } else {
    std::cout << "Reading from file: " << inputFile << "\n";
    galois::graphs::readGraph(graph, inputFile);
  }
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges\n";

  if (startNode >= graph.size() || reportNode >= graph.size()) {
    std::cerr << "failed to set report: " << reportNode
              << " or failed to set source: " << startNode << "\n";
    assert(0);
    abort();
  }

  auto it = graph.begin();
  std::advance(it, startNode.getValue());
  source = *it;
  it     = graph.begin();
  std::advance(it, reportNode.getValue());
  report = *it;

  size_t approxNodeData = graph.size() * 64;
  galois::preAlloc(numThreads +
                   approxNodeData / galois::runtime::pagePoolSize());
  galois::reportPageAlloc("MeminfoPre");

  std::vector<unsigned int> sourceIds = readSourceIds(graph.size());

  std::cout << "Running " << ALGO_NAMES[algo] << " algorithm\n";

  namespace gwl = galois::worklists;
  using PSchunk4 = gwl::PerSocketChunkFIFO<4>;
  using PSchunk8 = gwl::PerSocketChunkFIFO<8>;
  using PSchunk16 = gwl::PerSocketChunkFIFO<16>;
  using PSchunk32 = gwl::PerSocketChunkFIFO<32>;
  using PSchunk64 = gwl::PerSocketChunkFIFO<64>;
  using PSchunk128 = gwl::PerSocketChunkFIFO<128>;
  using PSchunk256 = gwl::PerSocketChunkFIFO<256>;
  using OBIM4 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk4>;
  using OBIM8 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk8>;
  using OBIM16 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk16>;
  using OBIM32 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk32>;
  using OBIM64 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk64>;
  using OBIM128 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk128>;
  using OBIM256 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk256>;
  using PMOD4 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk4>;
  using PMOD8 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk8>;
  using PMOD16 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk16>;
  using PMOD32 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk32>;
  using PMOD64 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk64>;
  using PMOD128 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk128>;
  using PMOD256 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk256>;

  galois::StatTimer autoAlgoTimer("AutoAlgo_0");
  galois::StatTimer execTime("Timer_0");
  execTime.start();

  // Wrapped in a lambda, capturing `source` by reference, so that it can be
  // re-run per source and per round with its body left untouched.
  auto runOnce = [&]() {
  switch (algo) {
  case deltaStepOBIM:
    std::cout << "running OBIM with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        deltaStepAlgo<UpdateRequest, OBIM4>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 8:
        deltaStepAlgo<UpdateRequest, OBIM8>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 16:
        deltaStepAlgo<UpdateRequest, OBIM16>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 32:
        deltaStepAlgo<UpdateRequest, OBIM32>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 64:
        deltaStepAlgo<UpdateRequest, OBIM64>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 128:
        deltaStepAlgo<UpdateRequest, OBIM128>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 256:
        deltaStepAlgo<UpdateRequest, OBIM256>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;
  case deltaStepPMOD:
    std::cout << "running PMOD with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        deltaStepAlgo<UpdateRequest, PMOD4>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 8:
        deltaStepAlgo<UpdateRequest, PMOD8>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 16:
        deltaStepAlgo<UpdateRequest, PMOD16>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 32:
        deltaStepAlgo<UpdateRequest, PMOD32>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 64:
        deltaStepAlgo<UpdateRequest, PMOD64>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 128:
        deltaStepAlgo<UpdateRequest, PMOD128>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      case 256:
        deltaStepAlgo<UpdateRequest, PMOD256>(
            graph, source, ReqPushWrap(), OutEdgeRangeFn{graph});
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;
  case MQ:
    std::cout << "running MQ\n";
    if (prefetch == 1) MQAlgo<true>(graph, source, mqWorkerThreads(), queueNum); 
    else MQAlgo<false>(graph, source, mqWorkerThreads(), queueNum); 
    break;
  case MQBucket:
    std::cout << "running MQBucket\n";
    if (prefetch == 1) MQAlgo<true>(graph, source, mqWorkerThreads(), queueNum); 
    else MQAlgo<false>(graph, source, mqWorkerThreads(), queueNum); 
    break;
  case serial:
    serialAlgo(graph, source);
    break;
  default:
    std::abort();
  }
  };

  for (size_t si = 0; si < sourceIds.size(); ++si) {
    auto sit = graph.begin();
    std::advance(sit, sourceIds[si]);
    source = *sit;
    std::cout << "\nsource = " << sourceIds[si] << std::endl;

    for (unsigned int round = 0; round < numRounds; ++round) {
      galois::do_all(galois::iterate(graph),
                     [&graph](GNode n) { graph.getData(n) = SSSP::DIST_INFINITY; });
      graph.getData(source) = 0;

      runOnce();

      reportReach(graph);
    }

    if (verifyEach) {
      if (SSSP::verify(graph, source)) {
        std::cout << "Verification successful.\n";
      } else {
        std::cerr << "ERROR: Verification failed.\n";
        abort();
      }
    }
  }

  execTime.stop();

  galois::reportPageAlloc("MeminfoPost");

  std::cout << "Node " << reportNode << " has distance "
            << graph.getData(report) << "\n";

  // Sanity checking code
  galois::GReduceMax<uint64_t> maxDistance;
  galois::GAccumulator<uint64_t> distanceSum;
  galois::GAccumulator<uint32_t> visitedNode;
  maxDistance.reset();
  distanceSum.reset();
  visitedNode.reset();

  galois::do_all(
      galois::iterate(graph),
      [&](uint64_t i) {
        weight_type myDistance = graph.getData(i);

        if (myDistance != SSSP::DIST_INFINITY) {
          maxDistance.update(myDistance);
          distanceSum += myDistance;
          visitedNode += 1;
        }
      },
      galois::loopname("Sanity check"), galois::no_stats());

  // report sanity stats
  uint64_t rMaxDistance = maxDistance.reduce();
  uint64_t rDistanceSum = distanceSum.reduce();
  uint64_t rVisitedNode = visitedNode.reduce();
  galois::gInfo("# visited nodes is ", rVisitedNode);
  galois::gInfo("Max distance is ", rMaxDistance);
  galois::gInfo("Sum of visited distances is ", rDistanceSum);

  if (!skipVerify) {
    if (SSSP::verify(graph, source)) {
      std::cout << "Verification successful.\n";
    } else {
      // GALOIS_DIE("verification failed");
      std::cout << "ERROR: Verification failed.\n";
    }
  }

  totalTime.stop();

  return 0;
}
