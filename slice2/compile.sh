
clang++ -O3 slice.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o slice

clang++ -O3 slice.cpp -DCONCURRENT -DPRINT_STATS -DNTHREADS=4 `llvm-config --cxxflags --ldflags --system-libs --libs core` -o slice-c

clang++ -O3 slice.cpp -DCSV -DRUN_COUNT=3 `llvm-config --cxxflags --ldflags --system-libs --libs core` -o slice-csv
