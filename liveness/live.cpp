#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef NTHREADS
#define NTHREADS 16
#endif

using namespace llvm;

std::mutex outsmtx;

struct TaskInfo {
  Function *func;
  size_t size;
  int index;

  bool operator<(const TaskInfo &rhs) const { return size < rhs.size; }
};

std::set<BasicBlock *> findExitBBs(Function &func) {
  std::set<BasicBlock *> exitBBs;
  for (auto &BB : func) {
    auto &lastI = BB.back();
    if (isa<ReturnInst>(lastI)) {
      exitBBs.insert(&BB);
    }
  }
  return exitBBs;
}

// PhiDefs(B) the variables defined by φ-functions at the entry of block B
// PhiUses(B) the set of variables used in a φ-function at the entry of a
// successor of the block B
void findUSEsDEFs(
    Function &func, std::unordered_map<BasicBlock *, std::set<Value *>> &USEs,
    std::unordered_map<BasicBlock *, std::set<Value *>> &DEFs,
    std::unordered_map<BasicBlock *, std::set<Value *>> &phiUSEs,
    std::unordered_map<BasicBlock *, std::set<Value *>> &phiDEFs) {
  for (auto &BB : func) {
    auto &DEF = DEFs[&BB];
    auto &USE = USEs[&BB];
    auto &pDEF = phiDEFs[&BB];

    auto iter = BB.begin();
    for (; iter != BB.end(); ++iter) {
      if (!isa<PHINode>(*iter))
        break;
      auto phi = dyn_cast<PHINode>(&*iter);
      pDEF.insert(phi);
      int numIncoming = phi->getNumIncomingValues();
      for (int i = 0; i < numIncoming; ++i) {

        Value *inVal = phi->getIncomingValue(i);
        BasicBlock *inBB = phi->getIncomingBlock(i);
        if (!isa<Instruction>(inVal) && !isa<Argument>(inVal))
          continue;
        phiUSEs[inBB].insert(inVal);
      }
    }

    for (; iter != BB.end(); ++iter) {
      auto &inst = *iter;
      // if (inst.mayHaveSideEffects())
      //   sideBBs.insert(&BB);

      for (auto &oprand : inst.operands()) {
        Value *val = oprand.get();
        if (isa<Instruction>(val) || isa<Argument>(val)) {
          if (DEF.find(val) == DEF.end()) {
            USE.insert(val); // use without def
          }
        }
      }
      if (!inst.getType()->isVoidTy()) {
        DEF.insert(&inst);
      }
    }
  }
}

void findLiveVars(Function &func,
                  std::unordered_map<BasicBlock *, std::set<Value *>> &INs,
                  std::unordered_map<BasicBlock *, std::set<Value *>> &OUTs) {
  if (func.isDeclaration())
    return;

  std::unordered_map<BasicBlock *, std::set<Value *>> USEs, DEFs, phiUSEs,
      phiDEFs;
  std::set<BasicBlock *> sideBBs;
  findUSEsDEFs(func, USEs, DEFs, phiUSEs, phiDEFs);
  std::queue<BasicBlock *> worklist;
  std::unordered_set<BasicBlock *> hashWL;
  // auto exitBBs = findExitBBs(func);
  // for (BasicBlock *eBB : exitBBs) {
  //   if (hashWL.insert(eBB).second)
  //     worklist.push(eBB);
  // }
  // for (BasicBlock *sBB : sideBBs) {
  //   if (hashWL.insert(sBB).second)
  //     worklist.push(sBB);
  // }
  ReversePostOrderTraversal<Function *> RPOT(&func);
  for (BasicBlock *BB : RPOT) {
    if (hashWL.insert(BB).second)
      worklist.push(BB);
  }

  // std::unordered_map<BasicBlock *, std::set<Value *>> INs, OUTs;
  while (!worklist.empty()) {
    BasicBlock *BB = worklist.front();
    worklist.pop();
    hashWL.erase(BB);

    // LiveOut(B) = ⋃_S∈succs(B) (LiveIn(S) \ PhiDefs(S)) ∪ PhiUses(B)
    // LiveIn(B) = PhiDefs(B) ∪ UpwardExposed(B) ∪ (LiveOut(B) \ Defs(B))
    // std::set<Value *> oldIN = INs[BB], oldOUT = OUTs[BB];
    bool changed = false;
    std::set<Value *> liveIN, liveOUT;
    liveOUT = phiUSEs[BB];
    for (BasicBlock *succ : successors(BB)) {
      std::set_difference(INs[succ].begin(), INs[succ].end(),
                          phiDEFs[succ].begin(), phiDEFs[succ].end(),
                          std::inserter(liveOUT, liveOUT.end()));
    }
    changed |= (OUTs[BB] != liveOUT);
    OUTs[BB] = liveOUT;

    liveIN = phiDEFs[BB];
    std::set_difference(OUTs[BB].begin(), OUTs[BB].end(), DEFs[BB].begin(),
                        DEFs[BB].end(), std::inserter(liveIN, liveIN.end()));
    liveIN.insert(USEs[BB].begin(), USEs[BB].end());
    changed |= (INs[BB] != liveIN);
    INs[BB] = liveIN;

    if (changed) {
      for (BasicBlock *pred : predecessors(BB)) {
        if (hashWL.insert(pred).second)
          worklist.push(pred);
      }
    }
  }
}

void threadedLiveVars(
    std::mutex &Qmutex, std::priority_queue<TaskInfo> &taskQ,
    std::vector<std::unordered_map<BasicBlock *, std::set<Value *>>> &funcINs,
    std::vector<std::unordered_map<BasicBlock *, std::set<Value *>>> &funcOUTs,
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
#ifdef PSTATS
    auto sub_start = std::chrono::high_resolution_clock::now();
#endif
    findLiveVars(*func, funcINs[index], funcOUTs[index]);
#ifdef PSTATS
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

#ifdef PSTATS
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
    errs() << "Expect IR filename\n";
    exit(1);
  }
  LLVMContext context;
  SMDiagnostic smd;
  char *filename = argv[1];
  std::unique_ptr<Module> module = parseIRFile(filename, smd, context);
  if (!module) {
    errs() << "Cannot parse IR file\n";
    smd.print(filename, errs());
    exit(1);
  }

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<std::unordered_map<BasicBlock *, std::set<Value *>>> funcINs(
      module->size()),
      funcOUTs(module->size());
  outs() << module->size() << " function(s), ";

// #define LIVE_CONCURRENT
#ifdef LIVE_CONCURRENT
  outs() << "concurrent mode\n";
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
    threads.emplace_back(threadedLiveVars, std::ref(Qmutex), std::ref(taskQ),
                         std::ref(funcINs), std::ref(funcOUTs), i);
  }
  for (auto &t : threads) {
    t.join();
  }

#else
  outs() << "sequential mode\n";
  std::string csvname = std::string(argv[1]) + ".csv";
  std::ofstream csv(csvname);
  csv << "name,size,time(us)\n";
#ifndef RUN_COUNT
#define RUN_COUNT 1
#endif

  for (auto [i, func] : enumerate(*module)) {
    std::string fname = func.getName().str();
    size_t fsize = func.size();
    int tftime = 0;
    for (int r = 0; r < RUN_COUNT; ++r) {
      auto fstart = std::chrono::high_resolution_clock::now();
      findLiveVars(func, funcINs[i], funcOUTs[i]);
      auto fend = std::chrono::high_resolution_clock::now();
      auto ftime =
          std::chrono::duration_cast<std::chrono::microseconds>(fend - fstart)
              .count();
      tftime += ftime;
    }
    tftime /= RUN_COUNT;
    csv << fname << "," << fsize << "," << tftime << "\n";
  }
#endif

  outs() << funcINs.size() << "=" << funcOUTs.size() << " results\n";

  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " ms\n";

#ifndef NO_OUTPUT
  for (auto [i, func] : enumerate(*module)) {
    outs() << "\nFunction: " << func.getName().data() << "\n";
    for (auto &BB : func) {
      outs() << BB;
      outs() << "IN set: ----------------\n";
      for (auto in : funcINs[i][&BB]) {
        outs() << *in << "\n";
      }
      outs() << "---------------- :IN set\n";
      outs() << "OUT set: ++++++++++++++++\n";
      for (auto out : funcOUTs[i][&BB]) {
        outs() << *out << "\n";
      }
      outs() << "++++++++++++++++ :OUT set\n";
      // outs() << "DEF set: ****************\n";
      // for (auto def : DEFs[&BB]) {
      //   outs() << *def << "\n";
      // }
      // outs() << "**************** :DEF set\n";
      // outs() << "USE set: ****************\n";
      // for (auto use : USEs[&BB]) {
      //   outs() << *use << "\n";
      // }
      // outs() << "**************** :USE set\n";
      // outs() << "phiDEF: ****************\n";
      // for (auto def : phiDEFs[&BB]) {
      //   outs() << *def << "\n";
      // }
      // outs() << "**************** :phiDEF\n";
      // outs() << "phiUSE: ****************\n";
      // for (auto use : phiUSEs[&BB]) {
      //   outs() << *use << "\n";
      // }
      // outs() << "**************** :phiUSE\n";
    }
    outs() << "******************************** " << func.getName().data()
           << "\n";
  }
#endif
}