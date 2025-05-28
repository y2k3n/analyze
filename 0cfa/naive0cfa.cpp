#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
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
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

std::mutex outsmtx;

struct TaskInfo {
  Function *func;
  size_t size;
  int index;

  bool operator<(const TaskInfo &rhs) const { return size < rhs.size; }
};

struct LocalData {
  std::unordered_map<Instruction *, DenseSet<Value *>> callMap;
  std::unordered_map<Value *, DenseSet<Value *>> points2;
  std::unordered_set<Value *> visited;
};

void analyzePtr(Value *val, LocalData &localdata) {
  auto &callMap = localdata.callMap;
  auto &points2 = localdata.points2;
  auto &visited = localdata.visited;
  if (visited.find(val) != visited.end()) {
    return;
  }
  visited.insert(val);
  // if (points2.find(val) != points2.end()) {
  //   return;
  // }

  if (isa<Function>(val) || isa<Argument>(val)) {
    points2[val] = {val};

  } else if (auto *cast = dyn_cast<CastInst>(val)) {
    auto *src = cast->getOperand(0);
    analyzePtr(src, localdata);
    points2[cast] = points2[src];

  } else if (auto *phi = dyn_cast<PHINode>(val)) {
    for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
      Value *inval = phi->getIncomingValue(i);
      // if (isa<Instruction>(inval) || isa<Argument>(inval)) {
      analyzePtr(inval, localdata);
      points2[phi].insert(points2[inval].begin(), points2[inval].end());
      // }
    }

  } else if (auto *select = dyn_cast<SelectInst>(val)) {
    Value *tval = select->getTrueValue();
    Value *fval = select->getFalseValue();
    // if (isa<Instruction>(tval) || isa<Argument>(tval)) {
    analyzePtr(tval, localdata);
    points2[select].insert(points2[tval].begin(), points2[tval].end());
    // }
    // if (isa<Instruction>(fval) || isa<Argument>(fval)) {
    analyzePtr(fval, localdata);
    points2[select].insert(points2[fval].begin(), points2[fval].end());
    // }

  } else if (auto *load = dyn_cast<LoadInst>(val)) {
    auto *loadptr = load->getPointerOperand();
    analyzePtr(loadptr, localdata);
    points2[load] = points2[loadptr];
    for (auto *user : loadptr->users()) {
      if (auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand() == loadptr) {
          auto *stval = store->getValueOperand();
          analyzePtr(stval, localdata);
          points2[load].insert(points2[stval].begin(), points2[stval].end());
        }
      }
    }

  } else if (auto *global = dyn_cast<GlobalVariable>(val)) {
    points2[val] = {val};
    if (global->hasInitializer()) {
      Value *initval = global->getInitializer();
      analyzePtr(initval, localdata);
      points2[val].insert(points2[initval].begin(), points2[initval].end());
    }
    for (auto *user : global->users()) {
      if (auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand() == global) {
          Value *storedVal = store->getValueOperand();
          analyzePtr(storedVal, localdata);
          points2[val].insert(points2[storedVal].begin(),
                              points2[storedVal].end());
        }
      }
    }

  } else if (auto *gep = dyn_cast<GetElementPtrInst>(val)) {
    Value *baseptr = gep->getPointerOperand();
    analyzePtr(baseptr, localdata);
    points2[gep] = points2[baseptr];

  // } else if (auto *cexpr = dyn_cast<ConstantExpr>(val)) {
  //   auto *ceinst = cexpr->getAsInstruction();
  //   analyzePtr(ceinst, localdata);
  //   points2[cexpr] = points2[ceinst];
  //   points2.erase(ceinst);
  //   ceinst->deleteValue();

  } else {
    points2[val] = {val};
  }
}

void analyzeIntra(Function &func, LocalData &localdata) {
  auto &callMap = localdata.callMap;
  auto &points2 = localdata.points2;
  auto &visited = localdata.visited;

  for (auto &BB : func) {
    for (auto &inst : BB) {
      if (auto *call = dyn_cast<CallInst>(&inst)) {
        // auto *called = call->getCalledFunction();
        // if (called != nullptr) {
        //   // direct call
        //   callMap[call] = {called};
        // } else {
        // indirect
        auto *callptr = call->getCalledOperand();
        analyzePtr(callptr, localdata);
        callMap[call] = points2[callptr];
        // }
      }
    }
  }
}

void print(LocalData &localdata) {
  auto &callMap = localdata.callMap;
  auto &points2 = localdata.points2;
  auto &visited = localdata.visited;

  for (auto &[key, targets] : callMap) {
    outs() << *key << "\n->";
    for (auto *target : targets) {
      if (dyn_cast<Function>(target)) {
        outs() << "\t<" << target->getName() << ">\n";
      } else {
        outs() << "\t" << *target << "\n";
      }
    }
    if (targets.empty()) {
      outs() << "\tempty\n";
    }
    outs() << "\n";
    // outs().flush();
  }
}

void threadedSlice(std::mutex &Qmutex, std::priority_queue<TaskInfo> &taskQ,
                  int tid) {
  auto start = std::chrono::high_resolution_clock::now();
  int max_time = 0;
  int max_size = 0;
  int task_count = 0;
  int total_size = 0;
  int total_size_sq = 0;
  int total_time = 0;
  int total_time_sq = 0;

  LocalData localdata;
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

    analyzeIntra(*func, localdata);

#ifdef PRINT_STATS
    auto sub_end = std::chrono::high_resolution_clock::now();
    auto sub_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        sub_end - sub_start);
    int time = sub_duration.count();
    if (time > max_time) {
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

  int mean_size = (task_count > 0) ? total_size / task_count : 0;
  int var_size = (task_count > 0)
                     ? (total_size_sq / task_count) - (mean_size * mean_size)
                     : -(mean_size * mean_size);
  int mean_time = (task_count > 0) ? total_time / task_count : 0;
  int var_time = (task_count > 0)
                     ? (total_time_sq / task_count) - (mean_time * mean_time)
                     : -(mean_time * mean_time);

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

  outs() << "Intra-Procedual 0-CFA" << "\n";
  outs() << module->getFunctionList().size() << " function(s)\n";

#ifdef CSV
  std::string csvname = std::string(argv[1]) + ".csv";
  std::ofstream csv(csvname);
  csv << "name,size,inum,time(us)\n";
#ifndef RUN_COUNT
#define RUN_COUNT 1
#endif
#endif

  auto start = std::chrono::high_resolution_clock::now();

#ifndef CONCURRENT
  outs() << "Sequential mode\n";
  LocalData localdata;
  auto &callMap = localdata.callMap;
  auto &points2 = localdata.points2;
  auto &visited = localdata.visited;

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
      callMap.clear();
      points2.clear();
      visited.clear();

      auto fstart = std::chrono::high_resolution_clock::now();
#endif

      analyzeIntra(func, localdata);

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
    print();
    outs() << "******************************** " << func.getName() << "\n";
#endif
  }

#else

#ifndef NTHREADS
#define NTHREADS 4
#endif
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
    threads.emplace_back(threaded0CFA, std::ref(Qmutex), std::ref(taskQ), i);
  }
  for (auto &t : threads) {
    t.join();
  }

#endif

  auto end = std::chrono::high_resolution_clock::now();

  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " us\n";
}