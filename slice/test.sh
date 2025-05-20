clang -S -O0 -Xclang -disable-O0-optnone -emit-llvm test.c -o test.ll
opt -S -passes=mem2reg test.ll -o test-mem2reg.ll
