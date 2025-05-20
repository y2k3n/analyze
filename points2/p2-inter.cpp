#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/CFG.h"
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

#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

std::unordered_map<Value *, std::set<Value *>> pt;
// std::queue<std::pair<Value *, std::set<Value *>>> worklist;
DenseMap<Value *, std::set<Value *>> WLMap;
std::unordered_map<Value *, std::set<Value *>> PFG;
std::unordered_set<Value *> RM;

void worklistPush(Value *key, const std::set<Value *> &sset) {
  auto it = WLMap.find(key);
  if (it != WLMap.end()) {
    it->second.insert(sset.begin(), sset.end());
  } else {
    WLMap[key] = sset;
  }
}

void addEdge(Value *s, Value *t) {
  if (PFG[s].find(t) == PFG[s].end()) {
    PFG[s].insert(t);
    if (!pt[s].empty()) {
      worklistPush(t, pt[s]);
    }
  }
}

void propagate(Value *n, const std::set<Value *> &pts) {
  if (!pts.empty()) {
    pt[n].insert(pts.begin(), pts.end());
    for (auto *s : PFG[n]) {
      worklistPush(s, pts);
    }
  }
}

void addReachable(Function *func);
void initialize(Function &func) {
  for (auto &BB : func) {
    for (auto &inst : BB) {

      if (auto *alloca = dyn_cast<AllocaInst>(&inst)) {
        worklistPush(alloca, {alloca});

      } else if (auto *gep = dyn_cast<GetElementPtrInst>(&inst)) {
        worklistPush(gep, {gep});

      } else if (auto *phi = dyn_cast<PHINode>(&inst)) {
        for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
          Value *val = phi->getIncomingValue(i);
          if (isa<Instruction>(val) || isa<Argument>(val)) {
            addEdge(val, phi);
          }
        }

      } else if (auto *select = dyn_cast<SelectInst>(&inst)) {
        Value *tval = select->getTrueValue();
        Value *fval = select->getFalseValue();
        if (isa<Instruction>(tval) || isa<Argument>(tval)) {
          addEdge(tval, select);
        }
        if (isa<Instruction>(fval) || isa<Argument>(fval)) {
          addEdge(fval, select);
        }

      } else if (auto *cast = dyn_cast<CastInst>(&inst)) {
        Value *src = cast->getOperand(0);
        addEdge(src, cast);
      }

      else if (auto *call = dyn_cast<CallInst>(&inst)) {
        auto *cf = call->getCalledFunction();
        if (!cf || cf->isDeclaration())
          continue;
        for (int i = 0; i < call->arg_size(); ++i) {
          if (i < cf->arg_size()) {
            addEdge(call->getArgOperand(i), cf->getArg(i));
          }
        }
        if (!cf->getReturnType()->isVoidTy()) {
          for (auto &cfBB : *cf) {
            for (auto &cfinst : cfBB) {
              if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&cfinst)) {
                Value *retVal = ret->getReturnValue();
                if (retVal)
                  addEdge(retVal, call);
              }
            }
          }
        }
        addReachable(cf);
      }

      // iter end
    }
  }
}

void addReachable(Function *func) {
  // outs() << "Reach function: " << func->getName() << "\n";
  if (RM.find(func) != RM.end()) {
    // outs() << "Already exist\n";
    return;
  }
  RM.insert(func);
  // errs() << "Reach " << func->getName() << " (" << RM.size() << ")\n";
  // TODO: Sm ?????
  initialize(*func);
}

void solve() {
  while (!WLMap.empty()) {
    // errs() << "worklist size=" << worklist.size() << "\n";
    auto it = WLMap.begin();
    auto n = it->first;
    auto pts = it->second;
    WLMap.erase(it);

    std::set<Value *> delta;
    std::set_difference(pts.begin(), pts.end(), pt[n].begin(), pt[n].end(),
                        std::inserter(delta, delta.begin()));
    propagate(n, delta);

    for (auto *user : n->users()) {
      if (StoreInst *store = dyn_cast<StoreInst>(user)) {
        // *x = y (store y -> ptr x)
        if (store->getPointerOperand() == n) {
          Value *y = store->getValueOperand();
          if (isa<Instruction>(y) || isa<Argument>(y)) {
            for (Value *oi : delta) {
              addEdge(y, oi);
            }
          }
        }

      } else if (LoadInst *load = dyn_cast<LoadInst>(user)) {
        // y = *x (load ptr x -> y)
        if (load->getPointerOperand() == n) {
          Value *y = load;
          for (Value *oi : delta) {
            addEdge(oi, y);
          }
        }
      }
    }
    // iter end
  }
}

void print() {
  outs() << "Points-to Set:\n";
  outs() << "=================\n";
  for (auto &[p, points2] : pt) {
    outs() << "\n" << *p << "\n->";
    if (points2.empty()) {
      outs() << "\tno points-to target\n";
    } else {
      for (Value *v : points2) {
        outs() << "\t" << *v << "\n";
      }
    }
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

  Function *mainFunc = module->getFunction("main");
  if (!mainFunc) {
    outs() << "Cannot find main function.\n";
    return 0;
  }

  outs() << "Inter-Procedural Analysis" << "\n";
  errs() << module->getFunctionList().size() << " function(s)\n";
  addReachable(mainFunc);
  errs() << "Solving...\n";
  solve();
  // print();
}