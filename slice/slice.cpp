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
  worklist.push(root);
  slice.insert(root);

  while (!worklist.empty()) {
    auto inst = worklist.front();
    worklist.pop();

    if (auto *phi = dyn_cast<PHINode>(inst)) {
      for (int i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto *ival = phi->getIncomingValue(i);
        if (auto *ivalInst = dyn_cast<Instruction>(ival)) {
          if (slice.insert(ivalInst).second) {
            worklist.push(ivalInst);
          }
        }
        auto *iBB = phi->getIncomingBlock(i);
        Instruction *term = iBB->getTerminator();
        if (slice.insert(term).second) {
          worklist.push(term);
        }
      }
      continue;
    }
    // non phi case
    for (auto &use : inst->operands()) {
      if (auto *op = dyn_cast<Instruction>(use)) {
        if (slice.insert(op).second) {
          worklist.push(op);
        }
      }
    }
    for (BasicBlock *predBB : predecessors(inst->getParent())) {
      auto *term = predBB->getTerminator();
      if (slice.insert(term).second) {
        worklist.push(term);
      }
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
  // outs() << module->getFunctionList().size() << " function(s)\n";
  // for (auto &func : *module) {
  //   if (func.isDeclaration())
  //     continue;
  //   for (auto &BB : func) {
  //     for (auto &inst : BB) {
  //     }
  //   }
  // }
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
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  printSlice(*module, slice);
  outs() << "Analysis time: " << duration.count() << " us\n";
}