clang -S -O0 -Xclang -disable-O0-optnone -emit-llvm test.cpp -o test.ll
opt -S -passes=mem2reg test.ll -o test-mem2reg.ll
# ./live test-mem2reg.ll
./live-c test-mem2reg.ll
# echo "Running sequential..."
# time ./live sqlite3.ll #> out.txt 2>&1

# echo "Running concurrent..."
# time ./live-c sqlite3.ll #> out-c.txt 2>&1

# diff -u out.txt out-c.txt