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

#include <set>
#include <unordered_map>
#include <vector>
#include <chrono>

using namespace llvm;

std::unordered_map<Value *, Value *> ds_parent;
std::unordered_map<Value *, int> ds_rank;
std::unordered_map<Value *, Value *> points2;

Value *findDS(Value *x) {
  if (ds_parent.find(x) == ds_parent.end()) {
    ds_parent[x] = x;
    ds_rank[x] = 0;
    return x;
  }
  if (x != ds_parent[x]) {
    ds_parent[x] = findDS(ds_parent[x]);
  }
  return ds_parent[x];
}

void unionDS(Value *p, Value *q) {
  Value *x = findDS(p);
  Value *y = findDS(q);
  if (x == y)
    return;
  if (ds_rank[x] < ds_rank[y]) {
    ds_parent[x] = y;
  } else if (ds_rank[x] > ds_rank[y]) {
    ds_parent[y] = x;
  } else {
    ds_parent[y] = x;
    ds_rank[x]++;
  }
}

void printGroups() {
  std::unordered_map<Value *, std::vector<Value *>> groups;
  std::unordered_map<Value *, std::set<Value *>> gp2;
  for (auto [key, val] : ds_parent) {
    Value *root = findDS(key);
    groups[root].push_back(key);
  }
  for (auto &[key, group] : groups) {
    for (auto val : group) {
      if (points2.find(val) != points2.end()) {
        gp2[key].insert(findDS(points2[val]));
      }
    }
  }
  for (auto &[key, group] : groups) {
    outs() << "\nGroup " << key << ": {";
    for (auto val : group) {
      outs() << "\n" << *val;
    }
    outs() << "\n}\nPoints-to group(s): {";
    for (auto val : gp2[key]) {
      outs() << " " << val;
    }
    outs() << " }\n";
  }
}

void steensgaard(Instruction *inst) {
  if (auto *ac = dyn_cast<AllocaInst>(inst)) {
    findDS(ac);
    points2[ac] = ac;

  } else if (auto *ld = dyn_cast<LoadInst>(inst)) {
    // [p := *q] -> join(*p, **q)
    auto *q = ld->getPointerOperand();
    if (points2.find(q) == points2.end()) {
      findDS(q);
      findDS(ld);
      points2[q] = ld;
    } else {
      unionDS(points2[q], ld);
    }

  } else if (auto *st = dyn_cast<StoreInst>(inst)) {
    // [*p := q] -> join(**p, *q)
    auto *p = st->getPointerOperand();
    auto *q = st->getValueOperand();
    if (isa<Instruction>(q) || isa<Argument>(q)) {
      if (points2.find(p) == points2.end()) {
        findDS(p);
        findDS(q);
        points2[p] = q;
      } else {
        unionDS(points2[p], q);
      }
    }

  } else if (PHINode *phi = dyn_cast<PHINode>(inst)) {
    // join incoming ptrs with phi var
    for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
      auto *val = phi->getIncomingValue(i);
      if (isa<Instruction>(val) || isa<Argument>(val)) {
        unionDS(phi, val);
      }
    }

  } else if (auto *select = dyn_cast<SelectInst>(inst)) {
    Value *tval = select->getTrueValue();
    Value *fval = select->getFalseValue();
    if (isa<Instruction>(tval) || isa<Argument>(tval)) {
      unionDS(tval, select);
    }
    if (isa<Instruction>(fval) || isa<Argument>(fval)) {
      unionDS(fval, select);
    }

  } else if (auto *cast = dyn_cast<CastInst>(inst)) {
    Value *src = cast->getOperand(0);
    unionDS(src, cast);

  } else if (auto *call = dyn_cast<CallInst>(inst)) {
    auto *cf = call->getCalledFunction();
    if (!cf || cf->isDeclaration())
      return;
    for (int i = 0; i < call->arg_size(); ++i) {
      if (i < cf->arg_size()) {
        unionDS(call->getArgOperand(i), cf->getArg(i));
      }
    }
    if (!cf->getReturnType()->isVoidTy()) {
      for (auto &cfBB : *cf) {
        for (auto &cfinst : cfBB) {
          if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&cfinst)) {
            Value *retVal = ret->getReturnValue();
            if (retVal)
              unionDS(retVal, call);
          }
        }
      }
    }
  }
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

  outs() << "Steensgaard's Analysis\n";
  outs() << module->getFunctionList().size() << " function(s)\n";
  auto start = std::chrono::high_resolution_clock::now();
  for (auto &func : *module) {
    if (func.isDeclaration())
      continue;

    // ds_parent.clear();
    // ds_rank.clear();
    // points2.clear();

    for (auto &BB : func) {
      for (auto &inst : BB) {
        steensgaard(&inst);
      }
    }
    // outs() << "\nFunction: " << func.getName() << "\n";
    // printGroups();
    // outs() << "******************************** " << func.getName() << "\n";
  }
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " us\n";
#ifdef PRINT_RESULTS
  printGroups();
#endif
}