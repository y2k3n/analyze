#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <queue>
// #include <set>
// #include <unordered_map>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace llvm;

std::mutex outsmtx;

struct TaskInfo {
  Function *func;
  Value *val;
  size_t size;
  int index;

  bool operator<(const TaskInfo &rhs) const { return size < rhs.size; }
};

void backwardSlice(Value *root, std::unordered_set<Value *> &slice) {
  std::queue<Value *> worklist;

  auto add2Slice = [&](Value *i) {
    if (slice.insert(i).second) {
      worklist.push(i);
    }
  };

  slice.insert(root);
  worklist.push(root);

  while (!worklist.empty()) {
    auto *val = worklist.front();
    worklist.pop();

    if (auto *phi = dyn_cast<PHINode>(val)) {
      for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto *ival = phi->getIncomingValue(i);
        if (auto *ivalInst = dyn_cast<Instruction>(ival)) {
          add2Slice(ivalInst);
        }
        auto *iBB = phi->getIncomingBlock(i);
        Instruction *term = iBB->getTerminator();
        add2Slice(term);
      }
      continue;

    } else if (auto *select = dyn_cast<SelectInst>(val)) {
      Value *tval = select->getTrueValue();
      Value *fval = select->getFalseValue();
      if (auto *tvalInst = dyn_cast<Instruction>(tval)) {
        add2Slice(tvalInst);
      }
      if (auto *fvalInst = dyn_cast<Instruction>(fval)) {
        add2Slice(fvalInst);
      }

    } else if (auto *cast = dyn_cast<CastInst>(val)) {
      Value *src = cast->getOperand(0);
      if (auto *srcInst = dyn_cast<Instruction>(src)) {
        add2Slice(srcInst);
      }

      // } else if (auto *call = dyn_cast<CallInst>(inst)) {
      //   auto *cf = call->getCalledFunction();
      //   if (cf && !cf->isDeclaration()) {
      //     for (int i = 0; i < call->arg_size(); ++i) {
      //       if (i < cf->arg_size()) {
      //         auto *arg = cf->getArg(i);
      //         if (auto *argInst = dyn_cast<Instruction>(arg)) {
      //           add2Slice(argInst);
      //         }
      //       }
      //     }
      //     if (!cf->getReturnType()->isVoidTy()) {
      //       for (auto &cfBB : *cf) {
      //         for (auto &cfinst : cfBB) {
      //           if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&cfinst)) {
      //             add2Slice(ret);
      //           }
      //         }
      //       }
      //     }
      //   }

    } else if (auto *inst = dyn_cast<Instruction>(val)) {
      for (auto &use : inst->operands()) {
        if (auto *op = dyn_cast<Instruction>(use)) {
          add2Slice(op);
        }
      }

    } else {
      add2Slice(val);
    }

    // for (BasicBlock *predBB : predecessors(inst->getParent())) {
    //   auto *term = predBB->getTerminator();
    //   add2Slice(term);
    // }
    // iter end
  }
}

void forwardSlice(Value *root, std::unordered_set<Value *> &slice) {
  std::queue<Value *> worklist;

  auto add2Slice = [&](Value *i) {
    if (slice.insert(i).second) {
      worklist.push(i);
    }
  };

  slice.insert(root);
  worklist.push(root);

  while (!worklist.empty()) {
    auto *val = worklist.front();
    worklist.pop();

    for (auto *user : val->users()) {
      add2Slice(user);
    }
  }
}

void printSlice(Module &module, std::unordered_set<Value *> &slice) {
  for (Function &func : module) {
    outs() << "Function: " << func.getName() << "\n";
    for (BasicBlock &BB : func) {
      for (Instruction &inst : BB) {
        outs() << inst;
        if (slice.count(&inst)) {
          outs() << "\t[slice]";
        }
        outs() << "\n";
      }
    }
  }
}

void sliceFunc(Function &func) {
  for (auto &BB : func) {
    for (auto &inst : BB) {
      if (isa<GetElementPtrInst>(inst)) {
        std::unordered_set<Value *> slice;
        backwardSlice(&inst, slice);
        forwardSlice(&inst, slice);
      } else if (isa<AllocaInst>(inst)) {
        std::unordered_set<Value *> slice;
        forwardSlice(&inst, slice);
      }
    }
  }
  for (auto &arg : func.args()) {
    std::unordered_set<Value *> slice;
    forwardSlice(&arg, slice);
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

  while (true) {
    int index;
    Function *func;
    Value *val;
    int size;
    {
      std::lock_guard<std::mutex> lock(Qmutex);
      if (taskQ.empty())
        break;
      index = taskQ.top().index;
      func = taskQ.top().func;
      val = taskQ.top().val;
      // size = taskQ.top().size;
      taskQ.pop();
    }
#ifdef PRINT_STATS
    auto sub_start = std::chrono::high_resolution_clock::now();
#endif

    std::unordered_set<Value *> slice;
    if (isa<GetElementPtrInst>(val)) {
      backwardSlice(val, slice);
      forwardSlice(val, slice);
    } else {
      forwardSlice(val, slice);
    }
    size = slice.size();

#ifdef PRINT_STATS
    auto sub_end = std::chrono::high_resolution_clock::now();
    auto sub_duration = std::chrono::duration_cast<std::chrono::microseconds>(
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
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  double mean_size = (task_count > 0) ? total_size / task_count : 0;
  double var_size = (task_count > 0)
                        ? (total_size_sq / task_count) - (mean_size * mean_size)
                        : -(mean_size * mean_size);
  double mean_time = (task_count > 0) ? total_time / task_count : 0;
  double var_time = (task_count > 0)
                        ? (total_time_sq / task_count) - (mean_time * mean_time)
                        : -(mean_time * mean_time);

  {
    std::lock_guard<std::mutex> lock(outsmtx);
    outs() << "\nThread " << tid << "\ttime:\t" << duration.count() << " us\n";
    outs() << "Max task time :\t " << max_time << " us with\t " << max_size
           << " instuctions in slice\n";
    outs() << "Tasks processed:\t" << task_count << "\n";
    outs() << "Task size mean:\t" << mean_size << ", var:\t" << var_size
           << ", std dev:\t" << std::sqrt(var_size) << "\n";
    outs() << "Task time mean:\t" << mean_time << ", var:\t" << var_time
           << ", std dev:\t" << std::sqrt(var_time) << "\n";
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

#ifdef CSV
  std::string csvname = std::string(argv[1]) + ".csv";
  std::ofstream csv(csvname);
  csv << "name,size,inum,time(us)\n";
#ifndef RUN_COUNT
#define RUN_COUNT 1
#endif
#endif

  outs() << "Slicing\n";
  outs() << module->getFunctionList().size() << " function(s)\n";
  auto start = std::chrono::high_resolution_clock::now();

// #define CONCURRENT
#ifndef CONCURRENT
  outs() << "Sequential mode\n";
  for (auto &func : *module) {
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
      sliceFunc(func);
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
    for (auto &BB : func) {
      for (auto &inst : BB) {
        if (isa<GetElementPtrInst>(inst)) {
          taskQ.push({&func, &inst, func.size(), (int)i});
        } else if (isa<AllocaInst>(inst)) {
          taskQ.push({&func, &inst, func.size(), (int)i});
        }
      }
    }
    for (auto &arg : func.args()) {
      taskQ.push({&func, &arg, func.size(), (int)i});
    }
  }

  std::mutex Qmutex;
  std::vector<std::thread> threads;
  threads.reserve(NTHREADS);
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back(threadedSlice, std::ref(Qmutex), std::ref(taskQ), i);
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