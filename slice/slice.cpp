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
#include <unordered_set>
// #include <vector>

using namespace llvm;

std::unordered_set<Value *> sliceInst(Instruction *root) {
  std::unordered_set<Value *> slice;
  std::queue<Instruction *> worklist;

  auto add2Slice = [&](Instruction *i) {
    if (slice.insert(i).second) {
      worklist.push(i);
    }
  };

  add2Slice(root);

  while (!worklist.empty()) {
    auto *inst = worklist.front();
    worklist.pop();

    if (auto *phi = dyn_cast<PHINode>(inst)) {
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

    } else if (auto *select = dyn_cast<SelectInst>(inst)) {
      Value *tval = select->getTrueValue();
      Value *fval = select->getFalseValue();
      if (auto *tvalInst = dyn_cast<Instruction>(tval)) {
        add2Slice(tvalInst);
      }
      if (auto *fvalInst = dyn_cast<Instruction>(fval)) {
        add2Slice(fvalInst);
      }

    } else if (auto *cast = dyn_cast<CastInst>(inst)) {
      Value *src = cast->getOperand(0);
      if (auto *srcInst = dyn_cast<Instruction>(src)) {
        add2Slice(srcInst);
      }

    } else if (auto *call = dyn_cast<CallInst>(inst)) {
      auto *cf = call->getCalledFunction();
      if (cf && !cf->isDeclaration()) {
        for (int i = 0; i < call->arg_size(); ++i) {
          if (i < cf->arg_size()) {
            auto *arg = cf->getArg(i);
            if (auto *argInst = dyn_cast<Instruction>(arg)) {
              add2Slice(argInst);
            }
          }
        }
        if (!cf->getReturnType()->isVoidTy()) {
          for (auto &cfBB : *cf) {
            for (auto &cfinst : cfBB) {
              if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&cfinst)) {
                add2Slice(ret);
              }
            }
          }
        }
      }

    } else {
      for (auto &use : inst->operands()) {
        if (auto *op = dyn_cast<Instruction>(use)) {
          add2Slice(op);
        }
      }
    }

    for (BasicBlock *predBB : predecessors(inst->getParent())) {
      auto *term = predBB->getTerminator();
      add2Slice(term);
    }
    // iter end
  }

  return slice;
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

  outs() << "Slicing\n";
  outs() << module->getFunctionList().size() << " function(s)\n";
  auto start = std::chrono::high_resolution_clock::now();
  auto *mainFunc = module->getFunction("main");
  Instruction *ret = nullptr;
  for (auto &BB : *mainFunc) {
    for (auto &inst : BB) {
      if (isa<ReturnInst>(&inst)) {
        ret = &inst;
        break;
      }
    }
    if (ret)
      break;
  }
  auto slice = sliceInst(ret);
  printSlice(*module, slice);
  // for (auto &func : *module) {
  //   for (auto &BB : func) {
  //     for (auto &inst : BB) {
  //       auto slice = sliceInst(&inst);
  //     }
  //   }
  // }
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  outs() << "Analysis time: " << duration.count() << " us\n";
}