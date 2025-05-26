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
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

std::unordered_map<Instruction *, DenseSet<Value *>> callMap;
std::unordered_map<Value *, DenseSet<Value *>> points2;
std::unordered_set<Value *> visited;

void analyzePtr(Value *val) {
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
    analyzePtr(src);
    points2[cast] = points2[src];

  } else if (auto *phi = dyn_cast<PHINode>(val)) {
    for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
      Value *inval = phi->getIncomingValue(i);
      // if (isa<Instruction>(inval) || isa<Argument>(inval)) {
      analyzePtr(inval);
      points2[phi].insert(points2[inval].begin(), points2[inval].end());
      // }
    }

  } else if (auto *select = dyn_cast<SelectInst>(val)) {
    Value *tval = select->getTrueValue();
    Value *fval = select->getFalseValue();
    // if (isa<Instruction>(tval) || isa<Argument>(tval)) {
    analyzePtr(tval);
    points2[select].insert(points2[tval].begin(), points2[tval].end());
    // }
    // if (isa<Instruction>(fval) || isa<Argument>(fval)) {
    analyzePtr(fval);
    points2[select].insert(points2[fval].begin(), points2[fval].end());
    // }

  } else if (auto *load = dyn_cast<LoadInst>(val)) {
    auto *loadptr = load->getPointerOperand();
    analyzePtr(loadptr);
    points2[load] = points2[loadptr];
    for (auto *user : loadptr->users()) {
      if (auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand() == loadptr) {
          auto *stval = store->getValueOperand();
          analyzePtr(stval);
          points2[load].insert(points2[stval].begin(), points2[stval].end());
        }
      }
    }

  } else if (auto *global = dyn_cast<GlobalVariable>(val)) {
    points2[val] = {val};
    if (global->hasInitializer()) {
      Value *initval = global->getInitializer();
      analyzePtr(initval);
      points2[val].insert(points2[initval].begin(), points2[initval].end());
    }
    for (auto *user : global->users()) {
      if (auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand() == global) {
          Value *storedVal = store->getValueOperand();
          analyzePtr(storedVal);
          points2[val].insert(points2[storedVal].begin(),
                              points2[storedVal].end());
        }
      }
    }

  } else if (auto *gep = dyn_cast<GetElementPtrInst>(val)) {
    Value *baseptr = gep->getPointerOperand();
    analyzePtr(baseptr);
    points2[gep] = points2[baseptr];

  } else if (auto *cexpr = dyn_cast<ConstantExpr>(val)) {
    Instruction *ceinst = cexpr->getAsInstruction();
    analyzePtr(ceinst);
    points2[val] = points2[ceinst];
    ceinst->deleteValue();

  } else {
    points2[val] = {val};
  }
}

void analyzeIntra(Function &func) {
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
        analyzePtr(callptr);
        callMap[call] = points2[callptr];
        // }
      }
    }
  }
}

void print() {
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

  for (auto &func : *module) {
    if (func.isDeclaration())
      continue;
    // callMap.clear();
    // points2.clear();
    // visited.clear();
    analyzeIntra(func);
#ifdef PRINT_RESULTS
    outs() << "\nFunction: " << func.getName() << "\n";
    print();
    outs() << "******************************** " << func.getName() << "\n";
#endif
  }

  auto end = std::chrono::high_resolution_clock::now();

  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " us\n";
}