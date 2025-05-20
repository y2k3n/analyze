#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <cmath>
#include <string>
#include <fstream>

using namespace llvm;


std::mutex outsmtx;

struct TaskInfo {
  Function *func;
  size_t size;
  int index;

  bool operator<(const TaskInfo &rhs) const { return size < rhs.size; }
};

struct LocalData {
  std::unordered_map<Value *, std::set<Value *>> pt;
  std::queue<std::pair<Value *, std::set<Value *>>> worklist;
  std::unordered_map<Value *, std::set<Value *>> PFG;
};

void addEdge(Value *s, Value *t, LocalData &localdata) {
  auto& pt = localdata.pt;
  auto &worklist = localdata.worklist;
  auto& PFG = localdata.PFG;
  if (PFG[s].find(t) == PFG[s].end()) {
    PFG[s].insert(t);
    if (!pt[s].empty()) {
      worklist.push({t, pt[s]});
    }
  }
}

void propagate(Value *n, const std::set<Value *> &pts, LocalData &localdata) {
  auto &pt = localdata.pt;
  auto &worklist = localdata.worklist;
  auto &PFG = localdata.PFG;
  if (!pts.empty()) {
    pt[n].insert(pts.begin(), pts.end());
    for (auto *s : PFG[n]) {
      worklist.push({s, pts});
    }
  }
}

void initialize(Function &func, LocalData& localdata) {
  auto &worklist = localdata.worklist;
  for (auto &BB : func) {
    for (auto &inst : BB) {

      if (auto *alloca = dyn_cast<AllocaInst>(&inst)) {
        worklist.push({alloca, {alloca}});

      } else if (auto *gep = dyn_cast<GetElementPtrInst>(&inst)) {
        worklist.push({gep, {gep}});

      } else if (auto *phi = dyn_cast<PHINode>(&inst)) {
        for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
          Value *val = phi->getIncomingValue(i);
          if (isa<Instruction>(val) || isa<Argument>(val)) {
            addEdge(val, phi, localdata);
          }
        }

      } else if (auto *select = dyn_cast<SelectInst>(&inst)) {
        Value *tval = select->getTrueValue();
        Value *fval = select->getFalseValue();
        if (isa<Instruction>(tval) || isa<Argument>(tval)) {
          addEdge(tval, select, localdata);
        }
        if (isa<Instruction>(fval) || isa<Argument>(fval)) {
          addEdge(fval, select, localdata);
        }

      } else if (auto *cast = dyn_cast<CastInst>(&inst)) {
        Value *src = cast->getOperand(0);
        addEdge(src, cast, localdata);
      }
      // iter end
    }
  }
}

void solve(LocalData& localdata) {
  auto &pt = localdata.pt;
  auto &worklist = localdata.worklist;
  // auto &PFG = localdata.PFG;
  while (!worklist.empty()) {
    auto [n, pts] = worklist.front();
    worklist.pop();

    std::set<Value *> delta;
    std::set_difference(pts.begin(), pts.end(), pt[n].begin(), pt[n].end(),
                        std::inserter(delta, delta.begin()));
    propagate(n, delta, localdata);

    for (auto *user : n->users()) {
      if (StoreInst *store = dyn_cast<StoreInst>(user)) {
        // *x = y (store y -> ptr x)
        if (store->getPointerOperand() == n) {
          Value *y = store->getValueOperand();
          if (isa<Instruction>(y) || isa<Argument>(y)) {
            for (Value *oi : delta) {
              addEdge(y, oi, localdata);
            }
          }
        }

      } else if (LoadInst *load = dyn_cast<LoadInst>(user)) {
        // y = *x (load ptr x -> y)
        if (load->getPointerOperand() == n) {
          Value *y = load;
          for (Value *oi : delta) {
            addEdge(oi, y, localdata);
          }
        }
      }
    }
    // iter end
  }
}

void print(LocalData& localdata) {
  auto &pt = localdata.pt;
  auto &PFG = localdata.PFG;
  outs() << "Points-to Set:\n";
  outs() << "=================\n";
  for (auto &[p, points2] : pt) {
    outs() << *p << "\n->";
    for (Value *v : points2) {
      outs() << "\t" << *v << "\n";
    }
    outs() << "\n";
  }

  // outs() << "Pointer Flow Graph:\n";
  // outs() << "=================\n";
  // for (auto &[from, toSet] : PFG) {
  //   outs() << *from << "\n->";
  //   for (Value *to : toSet) {
  //     outs() << "\t" << *to << "\n";
  //   }
  //   outs() << "\n";
  // }
}

void threadedPoints2(std::mutex &Qmutex, std::priority_queue<TaskInfo> &taskQ,
                      int tid) {
  auto start = std::chrono::high_resolution_clock::now();
  int max_time = 0;
  int max_size = 0;
  int task_count = 0;
  int total_size = 0;
  int total_size_sq = 0;
  int total_time = 0;
  int total_time_sq = 0;

  while (true) {
    int index;
    Function *func;
    int size;
    {
      std::lock_guard<std::mutex> lock(Qmutex);
      if (taskQ.empty())
        break;
      index = taskQ.top().index;
      func = taskQ.top().func;
      size = taskQ.top().size;
      taskQ.pop();
    }
#ifdef PRINT_STATS
    auto sub_start = std::chrono::high_resolution_clock::now();
#endif

    LocalData localdata;
    initialize(*func, localdata);
    solve(localdata);

#ifdef PRINT_STATS
    auto sub_end = std::chrono::high_resolution_clock::now();
    auto sub_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(sub_end -
        sub_start);
    int time = sub_duration.count();
    if (time > max_time){
      max_time = time;
      max_size = size;
    }
    task_count++;
    total_size += size;
    total_size_sq += size * size;
    total_time += time;
    total_time_sq += time * time;
#endif
  }

#ifdef PRINT_STATS
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  int mean_size = total_size / task_count;
  int var_size = (total_size_sq / task_count) - (mean_size * mean_size);
  int mean_time = total_time / task_count;
  int var_time = (total_time_sq / task_count) - (mean_time * mean_time);

  {
    std::lock_guard<std::mutex> lock(outsmtx);
    outs() << "\nThread " << tid << "\ttime:\t" << duration.count() << " ms\n";
    outs() << "Max task time :\t " << max_time << " ms with\t " << max_size
           << " BBs\n";
    outs() << "Tasks processed:\t" << task_count << "\n";
    outs() << "Task size mean:\t" << mean_size << ", var:\t" << var_size
           << ", std dev:\t" << (int)std::sqrt(var_size) << "\n";
    outs() << "Task time mean:\t" << mean_time << ", var:\t" << var_time
           << ", std dev:\t" << (int)std::sqrt(var_time) << "\n";
  }
#endif
}

int main(int argc, char *argv[]) {
  InitLLVM X(argc, argv);
  if (argc < 2) {
    outs() << "Expect IR filename\n";
    exit(1);
  }
  LLVMContext context;
  SMDiagnostic smd;
  char *filename = argv[1];
  std::unique_ptr<Module> module = parseIRFile(filename, smd, context);
  if (!module) {
    outs() << "Cannot parse IR file\n";
    smd.print(filename, outs());
    exit(1);
  }

  outs() << "Intra-Procedural Analysis" << "\n";
  outs() << module->getFunctionList().size() << " function(s)\n";
  auto start = std::chrono::high_resolution_clock::now();

#ifndef NTHREADS
#define NTHREADS 16
#endif

// #define CONCURRENT
#ifdef CONCURRENT
  outs() << "Concurrent mode\n";
  std::priority_queue<TaskInfo> taskQ;
  for (auto [i, func] : enumerate(*module)) {
    if (func.isDeclaration())
      continue;
    taskQ.push({&func, func.size(), (int)i});
  }
  std::mutex Qmutex;
  std::vector<std::thread> threads;
  threads.reserve(NTHREADS);
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back(threadedPoints2, std::ref(Qmutex), std::ref(taskQ), i);
  }
  for (auto &t : threads) {
    t.join();
  }

#else
  outs() << "Sequential mode\n";

// #define CSV
#ifdef CSV
  std::string csvname = std::string(argv[1]) + ".csv";
  std::ofstream csv(csvname);
  csv << "name,size,inum,time(us)\n";
#ifndef RUN_COUNT
#define RUN_COUNT 1
#endif
#endif

  for (auto &func : *module) {
    if (func.isDeclaration())
      continue;
#ifdef CSV
    std::string fname = func.getName().str();
    size_t fsize = func.size();
    int instNum = 0;
    for (BasicBlock &BB : func) {
      instNum += BB.size();
    }
    int tftime = 0;
    for (int r = 0; r < RUN_COUNT; ++r) {
      auto fstart = std::chrono::high_resolution_clock::now();
#endif
      LocalData localdata;
      initialize(func, localdata);
      solve(localdata);
#ifdef CSV
      auto fend = std::chrono::high_resolution_clock::now();
      auto ftime =
          std::chrono::duration_cast<std::chrono::microseconds>(fend - fstart)
              .count();
      tftime += ftime;
    }
    tftime /= RUN_COUNT;
    csv << fname << "," << fsize << "," << instNum << "," << tftime << "\n";
#endif

#ifdef PRINT_RESULTS
    outs() << "\nFunction: " << func.getName() << "\n";
    print(localdata);
    outs() << "******************************** " << func.getName() << "\n";
#endif
  }
#endif

  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " us\n";
}