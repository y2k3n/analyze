
clang++ -O3 naive0cfa.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o naive0cfa

clang++ -O3 naive0cfa.cpp -DCONCURRENT -DNTHREADS=4 -DPRINT_STATS `llvm-config --cxxflags --ldflags --system-libs --libs core` -o naive0cfa-c

clang++ -O3 naive0cfa.cpp -DCSV -DRUN_COUNT=3 `llvm-config --cxxflags --ldflags --system-libs --libs core` -o naive0cfa-csv
