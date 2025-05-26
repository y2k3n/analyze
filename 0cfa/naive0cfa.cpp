#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <llvm-19/llvm/Support/Casting.h>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

std::unordered_map<Instruction *, DenseSet<Value *>> callMap;
std::unordered_map<Value *, DenseSet<Value *>> points2;

void analyzePtr(Function& func, Value *val) {
  if (points2.find(val) != points2.end()) {
    return;
  }

  if (isa<Function>(val) || isa<Argument>(val)) {
    points2[val]={val};

  } else if (auto *cast = dyn_cast<CastInst>(val)) {
    auto *src = cast->getOperand(0);
    analyzePtr(func, src);
    points2[cast] = points2[src];

  } else if (auto *phi = dyn_cast<PHINode>(val)) {
    for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
      Value *inval = phi->getIncomingValue(i);
      if (isa<Instruction>(inval) || isa<Argument>(inval)) {
        analyzePtr(func, inval);
        points2[phi].insert(points2[inval].begin(), points2[inval].end());
      }
    }

  } else if (auto *select = dyn_cast<SelectInst>(val)) {
    Value *tval = select->getTrueValue();
    Value *fval = select->getFalseValue();
    // if (isa<Instruction>(tval) || isa<Argument>(tval)) {
    analyzePtr(func, tval);
    points2[select].insert(points2[tval].begin(), points2[tval].end());
    // }
    // if (isa<Instruction>(fval) || isa<Argument>(fval)) {
    analyzePtr(func, fval);
    points2[select].insert(points2[fval].begin(), points2[fval].end());
    // }

  } else if (auto *load = dyn_cast<LoadInst>(val)) {
    auto *loadptr = load->getPointerOperand();
    for (auto &BB : func) {
      for (auto &inst : BB) {
        if (auto *store = dyn_cast<StoreInst>(&inst)) {
          if (store->getPointerOperand() == loadptr) {
            auto *stval = store->getValueOperand();
            analyzePtr(func, stval);
            points2[load].insert(points2[stval].begin(),
                                   points2[stval].end());
          }
        }
      }
    }

  } else {
    points2[val]={val};
  }
}

void analyzeIntra(Function &func) {
  for (auto &BB : func) {
    for (auto &inst : BB) {
      if (auto *call = dyn_cast<CallInst>(&inst)) {
        auto *called = call->getCalledFunction();
        if (called != nullptr) {
          // direct call
          callMap[call] = {called};
        } else {
          // indirect
          auto *callptr = call->getCalledOperand();
          analyzePtr(func, callptr);
          callMap[call] = points2[callptr];
        }
      }
    }
  }
}

void print(){
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
  auto start = std::chrono::high_resolution_clock::now();

  for (auto& func : *module){
    if (func.isDeclaration())
      continue;
    callMap.clear();
    points2.clear();
    analyzeIntra(func);

    outs() << "\nFunction: " << func.getName() << "\n";
    print();
    outs() << "******************************** " << func.getName() << "\n";
  }

  auto end = std::chrono::high_resolution_clock::now();

  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " us\n";

#ifdef PRINT_RESULTS
  print();
#endif
}