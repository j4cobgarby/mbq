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

#include "Lonestar/BoilerPlate.h"
#include "PageRank-constants.h"
#include "galois/Bag.h"
#include "galois/Galois.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "include/BucketStructs.h"
#include "include/MultiBucketQueue.h"
#include "include/MultiQueue.h"

// #define PERF 1

/**
 * These implementations are based on the Push-based PageRank computation
 * (Algorithm 4) as described in the PageRank Europar 2015 paper.
 *
 * WHANG, Joyce Jiyoung, et al. Scalable data-driven pagerank: Algorithms,
 * system issues, and lessons learned. In: European Conference on Parallel
 * Processing. Springer, Berlin, Heidelberg, 2015. p. 438-450.
 */

const char* desc =
    "Computes page ranks a la Page and Brin. This is a push-style algorithm.";

static cll::opt<unsigned int>
    stepShift("delta",
              cll::desc("Shift value for the deltastep (default value 13)"),
              cll::init(13));
static cll::opt<unsigned int>
    threadNum("threads",
              cll::desc("number of threads for MQ"),
              cll::init(1));
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
              cll::init(1));


constexpr static const unsigned CHUNK_SIZE = 512;

constexpr static const unsigned MAX_PREFETCH_DEG = 1024;

enum Algo { OBIMPR, PMODPR, MQ, MQBucket };

static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
                           cll::values(clEnumVal(OBIMPR, "OBIMPR"),
                                       clEnumVal(PMODPR, "PMODPR"),
                                       clEnumVal(MQ, "MQ"),
                                       clEnumVal(MQBucket, "MQBucket")),
                           cll::init(OBIMPR));

struct LNode {
  PRTy value;
  std::atomic<PRTy> residual;

  void init() {
    value    = 0.0;
    residual = INIT_RESIDUAL;
  }

  friend std::ostream& operator<<(std::ostream& os, const LNode& n) {
    os << "{PR " << n.value << ", residual " << n.residual << "}";
    return os;
  }
};

typedef galois::graphs::LC_CSR_Graph<LNode, void>::with_numa_alloc<
    true>::type ::with_no_lockable<true>::type Graph;
typedef typename Graph::GraphNode GNode;

// -------------------------- OBIM / PMOD Implementation ------------------

float __attribute__ ((noinline)) addResidual(std::atomic<float>& v, float delta) {
  return atomicAdd(v, delta);
}

// For converting between float residual to integer priority levels
static constexpr uint64_t MULT_VAL = 1e7;
static constexpr float INV_MULT_VAL = 1.0 / MULT_VAL;

struct UpdateRequest {
  uint32_t src;
  uint32_t prio;
  UpdateRequest(const uint32_t& N, uint32_t D) : src(N), prio(D) {}
};

struct UpdateRequestIndexer {
  uint32_t shift;
  uint32_t operator()(const UpdateRequest& req) const {
    uint32_t t = req.prio >> shift;
    return t;
  }
};

template <typename OBIMTy>
void deltaPageRank(Graph& graph) {
  std::cout << "\n";
  std::cout << "delta = " << stepShift << "\n";

  // thread-local counters
  galois::GAccumulator<size_t> WLEmptyWork;

  // Insert all the vertices with their initial residual
  galois::InsertBag<UpdateRequest> initBag;
  galois::do_all(
      galois::iterate(graph),
      [&](const GNode& src) {
        constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        LNode& sdata = graph.getData(src, flag);
        float res = sdata.residual.load(std::memory_order_relaxed);
        int src_nout = std::distance(graph.edge_begin(src, flag),
                                      graph.edge_end(src, flag));
        if (src_nout == 0) src_nout = 1;
        uint32_t prio = res / src_nout * MULT_VAL;
        initBag.push(UpdateRequest(src, prio));
      },
      galois::no_stats());

  auto begin = std::chrono::high_resolution_clock::now();
  galois::for_each(
      galois::iterate(initBag),
      [&](const UpdateRequest& item, auto& ctx) {
        uint32_t src = item.src;
        uint32_t pushedPrioInt = item.prio;
        float pushedPrio = pushedPrioInt * INV_MULT_VAL;
        LNode& sdata = graph.getData(src);
        constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

        float curRes = sdata.residual.load(std::memory_order_relaxed);
        if (curRes <= tolerance || curRes < pushedPrio) {
          WLEmptyWork += 1;
          return;
        }

        // Clear residual and add to src's pagerank value
        PRTy oldResidual = sdata.residual.exchange(0.0, std::memory_order_acq_rel);
        sdata.value += oldResidual;
        int src_nout = std::distance(graph.edge_begin(src, flag),
                                      graph.edge_end(src, flag));
        if (src_nout == 0) {
          WLEmptyWork += 1;
          return;
        }
        
        PRTy delta = oldResidual * ALPHA / src_nout;
        if (delta == 0) {
          WLEmptyWork += 1;
          return;
        }

        // For each out-going neighbors, add the delta to their residuals,
        // potentially enqueue a new task for it
        for (auto jj : graph.edges(src, flag)) {
          GNode dst    = graph.getEdgeDst(jj);
          LNode& ddata = graph.getData(dst, flag);
#ifdef PERF
          auto old = addResidual(ddata.residual, delta);
#else
          auto old = atomicAdd(ddata.residual, delta);
#endif
          auto newRes = old + delta;
          if (newRes < tolerance) continue;

          int dst_nout = std::distance(graph.edge_begin(dst, flag),
                                        graph.edge_end(dst, flag));
          if (dst_nout == 0) dst_nout = 1;
          uint32_t oldDeltaPrio = uint32_t(old / dst_nout * MULT_VAL) >> stepShift;
          uint32_t newPrio = newRes / dst_nout * MULT_VAL;
          uint32_t newDeltaPrio = newPrio >> stepShift;

          // The original algorithm only enqueues 1 task per vertex 
          // if the residual was previously below tolerance,
          // and accumulates above tolerance at this instant.
          // The priority of that task is based off of
          // the residual seen at that instant.
          // 
          // We allow multiple tasks to be enqueued for the same vertex,
          // but only when its residual is above tolerance,
          // and have accumulated enough change.
          // Each vertex allows at most 1 task per priority level (delta) 
          // to reduce the number of enqueues.
          if ((old >= tolerance && oldDeltaPrio != newDeltaPrio) ||
              (old < tolerance && newRes >= tolerance))
            ctx.push(UpdateRequest(dst, newPrio));
        }
      },
      galois::wl<OBIMTy>(UpdateRequestIndexer{stepShift}), // OBIM worklist
      galois::disable_conflict_detection(), galois::loopname("PR"));

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  std::cout << "runtime_ms " << ms << "\n";

  galois::runtime::reportStat_Single("PageRank", "WLEmptyWork", WLEmptyWork.reduce());
}

// -------------------------- MQ Bucket Implementation ------------------

struct stat {
  uint64_t iter = 0;
  uint64_t emptyWork = 0;
};

template<typename MQ_Bucket>
void MQBucketThreadTask(Graph& graph, MQ_Bucket &wl, stat *stats) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  uint32_t src, b;
  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
  wl.initTID();

  while (true) {
    auto item = wl.pop();
    if (item) std::tie(b, src) = item.get();
    else break;

    iter++;

    float pushedPrio = (float)b * INV_MULT_VAL;

    LNode& sdata = graph.getData(src);
    float curRes = sdata.residual.load(std::memory_order_relaxed);
    if (curRes <= tolerance || curRes < pushedPrio) {
      emptyWork++;
      continue;
    }

    // Clear residual and add to src's pagerank value
    PRTy oldResidual = sdata.residual.exchange(0.0, std::memory_order_acq_rel);
    sdata.value += oldResidual;
    int src_nout = std::distance(graph.edge_begin(src, flag),
                                  graph.edge_end(src, flag));
    if (src_nout == 0) {
      emptyWork++;
      continue;
    }

    PRTy delta = oldResidual * ALPHA / src_nout;
    if (delta == 0) {
      emptyWork++;
      continue;
    }

    // For each out-going neighbors, add the delta to their residuals,
    // potentially enqueue a new task for it
    for (auto jj : graph.edges(src, flag)) {
      GNode dst    = graph.getEdgeDst(jj);
      LNode& ddata = graph.getData(dst, flag);
#ifdef PERF
      auto old = addResidual(ddata.residual, delta);
#else
      auto old = atomicAdd(ddata.residual, delta);
#endif
      auto newRes = old + delta;
      if (newRes < tolerance) continue;

      int dst_nout = std::distance(graph.edge_begin(dst, flag),
                                    graph.edge_end(dst, flag));
      if (dst_nout == 0) dst_nout = 1;
      mbq::BucketID oldBkt = mbq::BucketID(old / dst_nout * MULT_VAL) >> stepShift;
      mbq::BucketID newBkt = mbq::BucketID(newRes / dst_nout * MULT_VAL) >> stepShift;

      // The original algorithm only enqueues 1 task per vertex 
      // if the residual was previously below tolerance,
      // and accumulates above tolerance at this instant.
      // The priority of that task is based off of
      // the residual seen at that instant.
      // 
      // We allow multiple tasks to be enqueued for the same vertex,
      // but only when its residual is above tolerance,
      // and have accumulated enough change.
      // Each vertex allows at most 1 task per priority level (delta) 
      // to reduce the number of enqueues.
      if ((old >= tolerance && oldBkt != newBkt) || 
          (old < tolerance && newRes >= tolerance)) {
        float prio = newRes / dst_nout * MULT_VAL;
        wl.push(prio, dst);
      }
    }
  }

  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<bool usePrefetch=true>
void MQBucketPageRank(Graph& graph) {
  uint threads = threadNum;
  uint queues = queueNum;
  std::cout << "\n";
  std::cout << "threads = " << threadNum << "\n";
  std::cout << "queues = " << queueNum << "\n";
  std::cout << "batchSizePop = " << batch1 << "\n";
  std::cout << "batchSizePush = " << batch2 << "\n";
  std::cout << "delta " << stepShift << "\n";
  std::cout << "stickiness = " << stickiness << "\n";
  std::cout << "prefetch " << usePrefetch << "\n";

  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  // Lambda for mapping a priority to a priority level
  std::function<mbq::BucketID(uint32_t)> getBucketID = [&] (uint32_t v) -> mbq::BucketID {
    LNode& sdata = graph.getData(v, flag);
    int nout = std::distance(graph.edge_begin(v, flag),
                              graph.edge_end(v, flag));
    float res = sdata.residual.load(std::memory_order_acquire);
    if (nout == 0) nout = 1;
    return mbq::BucketID(res / nout * MULT_VAL) >> stepShift;
  };
  mbq::BucketID maxInitBkt = 0;
  for (uint i = 0; i < graph.size(); i++) {
    mbq::BucketID bkt = getBucketID(i);    
    if (bkt > maxInitBkt) maxInitBkt = bkt;
  }

  // Prefetcher lambda for reducing cache misses on load to a 
  // vertex's distance
  std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
    if (graph.getDegree(v) > MAX_PREFETCH_DEG) return;
    LNode& sdata = graph.getData(v, flag);
    __builtin_prefetch(&sdata.residual, 1, 3);
  };
  
  using MQ_Bucket = mbq::MultiBucketQueue<decltype(getBucketID), decltype(prefetcher), std::less<mbq::BucketID>, uint32_t, uint32_t, usePrefetch>;
  MQ_Bucket wl(getBucketID, prefetcher, queues, threads, stepShift, bucketNum, batch1, batch2, mbq::decreasing, stickiness, maxInitBkt);
  
  // Insert all the vertices with their initial residual
  for (uint i = 0; i < graph.size(); i++) {
    LNode& sdata = graph.getData(i, flag);
    int nout = std::distance(graph.edge_begin(i, flag),
                              graph.edge_end(i, flag));
    float res = sdata.residual.load(std::memory_order_relaxed);
    if (nout == 0) nout = 1;
    uint32_t prio = res / nout * MULT_VAL;
    wl.push(prio, i);
  }
  std::cout << "max init bkt = " << maxInitBkt << "\n";

  stat stats[threads];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (uint i = 1; i < threads; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = i;
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      MQBucketThreadTask<MQ_Bucket>, std::ref(graph), 
      std::ref(wl), &stats[i]);
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
  MQBucketThreadTask<MQ_Bucket>(graph, wl, &stats[0]);
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  if (!wl.empty()) {
    std::cout << "not empty\n";
  }

  wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (uint i = 0; i < threads; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "total iter = " << totalIter << "\n";
  std::cout << "total empty = " << totalEmptyWork << "\n";
}

// -------------------------- MQ Implementation ------------------

template<typename MQ_Type>
void MQThreadTask(Graph& graph, MQ_Type &wl, stat *stats) {
  uint64_t iter = 0UL;
  uint64_t emptyWork = 0UL;
  float pushedPrio;
  uint32_t src;
  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
  wl.initTID();

  while (true) {
    auto item = wl.pop();
    if (item) std::tie(pushedPrio, src) = item.get();
    else break;
    ++iter;

    LNode& sdata = graph.getData(src);
    float curRes = sdata.residual.load(std::memory_order_relaxed);
    if (curRes <= tolerance || curRes < pushedPrio) {
      emptyWork++;
      continue;
    }

    // Clear residual and add to src's pagerank value
    PRTy oldResidual = sdata.residual.exchange(0.0, std::memory_order_acq_rel);
    sdata.value += oldResidual;
    int src_nout = std::distance(graph.edge_begin(src, flag),
                                  graph.edge_end(src, flag));
    if (src_nout == 0) {
      emptyWork++;
      continue;
    }

    PRTy delta = oldResidual * ALPHA / src_nout;
    if (delta == 0) {
      emptyWork++;
      continue;
    }

    // For each out-going neighbors, add the delta to their residuals,
    // potentially enqueue a new task for it
    for (auto jj : graph.edges(src, flag)) {
      GNode dst    = graph.getEdgeDst(jj);
      LNode& ddata = graph.getData(dst, flag);
#ifdef PERF
      auto old = addResidual(ddata.residual, delta);
#else
      auto old = atomicAdd(ddata.residual, delta);
#endif
      auto newRes = old + delta;
      if (newRes < tolerance) continue;

      int dst_nout = std::distance(graph.edge_begin(dst, flag),
                                    graph.edge_end(dst, flag));
      if (dst_nout == 0) dst_nout = 1;

      float prio = newRes / dst_nout;
      uint32_t oldDeltaPrio = uint32_t(old / dst_nout * MULT_VAL) >> stepShift;
      uint32_t newDeltaPrio = uint32_t(prio * MULT_VAL) >> stepShift;

      // The original algorithm only enqueues 1 task per vertex 
      // if the residual was previously below tolerance,
      // and accumulates above tolerance at this instant.
      // The priority of that task is based off of
      // the residual seen at that instant.
      // 
      // We allow multiple tasks to be enqueued for the same vertex,
      // but only when its residual is above tolerance,
      // and have accumulated enough change.
      // Each vertex allows at most 1 task per priority level (delta) 
      // to reduce the number of enqueues.
      if ((old >= tolerance && oldDeltaPrio != newDeltaPrio) || 
          (old < tolerance && newRes >= tolerance)) {
        wl.push(prio, dst);
      }
    }
  }

  stats->iter = iter;
  stats->emptyWork = emptyWork;
}

template<bool usePrefetch=true>
void MQPageRank(Graph& graph) {
  uint threads = threadNum;
  uint queues = queueNum;
  std::cout << "\n";
  std::cout << "threads = " << threadNum << "\n";
  std::cout << "queues = " << queueNum << "\n";
  std::cout << "batchSizePop = " << batch1 << "\n";
  std::cout << "batchSizePush = " << batch2 << "\n";
  std::cout << "stickiness = " << stickiness << "\n";
  std::cout << "delta " << stepShift << "\n";
  std::cout << "prefetch " << usePrefetch << "\n";
  constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  // Prefetcher lambda for reducing cache misses on load to a 
  // vertex's distance
  std::function<void(uint32_t)> prefetcher = [&] (uint32_t v) -> void {
    if (graph.getDegree(v) > MAX_PREFETCH_DEG) return;
    LNode& sdata = graph.getData(v, flag);
    __builtin_prefetch(&sdata.residual, 1, 3);
  };

  using PQElement = std::tuple<float, uint32_t>;
  using MQ_Type = mbq::MultiQueue<decltype(prefetcher), std::less<PQElement>, float, uint32_t, usePrefetch>;
  MQ_Type wl(prefetcher, queues, threads, batch1, batch2, stickiness);

  // Insert all the vertices with their initial residual
  for (uint i = 0; i < graph.size(); i++) {
    LNode& sdata = graph.getData(i, flag);
    float res = sdata.residual.load(std::memory_order_relaxed);
    int nout = std::distance(graph.edge_begin(i, flag),
                              graph.edge_end(i, flag));
    if (nout == 0) nout = 1;
    float prio = res / nout;
    wl.push(prio, i);
  }

  stat stats[threads];
  auto begin = std::chrono::high_resolution_clock::now();

  std::vector<std::thread*> workers;
  cpu_set_t cpuset;
  for (uint i = 1; i < threads; i++) {
    CPU_ZERO(&cpuset);
    uint64_t coreID = i;
    CPU_SET(coreID, &cpuset);
    std::thread *newThread = new std::thread(
      MQThreadTask<MQ_Type>, std::ref(graph), 
      std::ref(wl), &stats[i]
    );
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
  MQThreadTask<MQ_Type>(graph, wl, &stats[0]);
  for (std::thread*& worker : workers) {
    worker->join();
    delete worker;
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
  if (!wl.empty()) {
    std::cout << "not empty\n";
  }

  wl.stat();
  std::cout << "runtime_ms " << ms << "\n";

  uint64_t totalIter = 0;
  uint64_t totalEmptyWork = 0;
  for (uint i = 0; i < threads; i++) {
    totalIter += stats[i].iter;
    totalEmptyWork += stats[i].emptyWork;
  }

  std::cout << "total iter = " << totalIter << "\n";
  std::cout << "total empty = " << totalEmptyWork << "\n";
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  galois::StatTimer totalTime("TimerTotal");
  totalTime.start();

  Graph graph;
  galois::graphs::readGraph(graph, inputFile);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges\n";

  galois::preAlloc(5 * numThreads +
                   (5 * graph.size() * sizeof(typename Graph::node_data_type)) /
                       galois::runtime::pagePoolSize());
  galois::reportPageAlloc("MeminfoPre");

  std::cout << "tolerance:" << tolerance << ", maxIterations:" << maxIterations
            << "\n";

  galois::do_all(
      galois::iterate(graph), [&graph](GNode n) { graph.getData(n).init(); },
      galois::no_stats(), galois::loopname("Initialize"));

  // if (algo != Async && algo != Sync) {
    std::cout << "initializing residual based on neighbor degrees\n";
    galois::do_all(
      galois::iterate(graph), 
      [&graph](GNode n) {
        constexpr const galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        LNode& sdata = graph.getData(n, flag);

        float res = 0;
        for (auto jj : graph.edges(n, flag)) {
          GNode dst = graph.getEdgeDst(jj);
          int dst_nout = std::distance(graph.edge_begin(dst, flag),
                                        graph.edge_end(dst, flag));
          if (dst_nout == 0) continue;
          res += 1.0 / dst_nout; 
        }

        res = ALPHA * INIT_RESIDUAL * res;
        sdata.residual.store(res, std::memory_order_relaxed);
      },
      galois::no_stats(), galois::loopname("Initialize residuals"));
  // }

  namespace gwl = galois::worklists;
  using PSchunk4 = gwl::PerSocketChunkFIFO<4>;
  using PSchunk8 = gwl::PerSocketChunkFIFO<8>;
  using PSchunk16 = gwl::PerSocketChunkFIFO<16>;
  using PSchunk32 = gwl::PerSocketChunkFIFO<32>;
  using PSchunk64 = gwl::PerSocketChunkFIFO<64>;
  using PSchunk128 = gwl::PerSocketChunkFIFO<128>;
  using PSchunk256 = gwl::PerSocketChunkFIFO<256>;
  using OBIM4 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk4>::with_descending<true>::type;
  using OBIM8 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk8>::with_descending<true>::type;
  using OBIM16 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk16>::with_descending<true>::type;
  using OBIM32 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk32>::with_descending<true>::type;
  using OBIM64 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk64>::with_descending<true>::type;
  using OBIM128 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk128>::with_descending<true>::type;
  using OBIM256 = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk256>::with_descending<true>::type;
  using PMOD4 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk4>::with_descending<true>::type;
  using PMOD8 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk8>::with_descending<true>::type;
  using PMOD16 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk16>::with_descending<true>::type;
  using PMOD32 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk32>::with_descending<true>::type;
  using PMOD64 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk64>::with_descending<true>::type;
  using PMOD128 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk128>::with_descending<true>::type;
  using PMOD256 = gwl::AdaptiveOrderedByIntegerMetric<UpdateRequestIndexer, PSchunk256>::with_descending<true>::type;

  galois::StatTimer execTime("Timer_0");
  execTime.start();

  switch (algo) {
  case MQ:
    std::cout << "running MQ\n";
    if (prefetch == 1) MQPageRank<true>(graph);
    else MQPageRank<false>(graph);
    break;

  case MQBucket:
    std::cout << "running MQBucket\n";
    if (prefetch == 1) MQBucketPageRank<true>(graph); 
    else MQBucketPageRank<false>(graph); 
    break;

  case OBIMPR:
    std::cout << "running OBIM with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        deltaPageRank<OBIM4>(graph);
        break;
      case 8:
        deltaPageRank<OBIM8>(graph);
        break;
      case 16:
        deltaPageRank<OBIM16>(graph);
        break;
      case 32:
        deltaPageRank<OBIM32>(graph);
        break;
      case 64:
        deltaPageRank<OBIM64>(graph);
        break;
      case 128:
        deltaPageRank<OBIM128>(graph);
        break;
      case 256:
        deltaPageRank<OBIM256>(graph);
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;

  case PMODPR:
    std::cout << "running PMOD with chunk size " << chunk << "\n";
    switch (chunk) {
      case 4:
        deltaPageRank<PMOD4>(graph);
        break;
      case 8:
        deltaPageRank<PMOD8>(graph);
        break;
      case 16:
        deltaPageRank<PMOD16>(graph);
        break;
      case 32:
        deltaPageRank<PMOD32>(graph);
        break;
      case 64:
        deltaPageRank<PMOD64>(graph);
        break;
      case 128:
        deltaPageRank<PMOD128>(graph);
        break;
      case 256:
        deltaPageRank<PMOD256>(graph);
        break;
      default:
        std::cerr << "ERROR: unkown chunk size\n";
    }
    break;

  default:
    std::abort();
  }

  execTime.stop();

  galois::reportPageAlloc("MeminfoPost");

  if (!skipVerify) {
    printTop(graph);
  }

#if DEBUG
  printPageRank(graph);
#endif

  totalTime.stop();

  return 0;
}
