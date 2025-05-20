
clang++ -O3 slice.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o slice
