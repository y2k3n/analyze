clang++ -O3 live.cpp -DNO_OUTPUT -DRUN_COUNT=3 `llvm-config --cxxflags --ldflags --system-libs --libs core` -o live

clang++ -O3 live.cpp -DNO_OUTPUT -DLIVE_CONCURRENT -DPSTATS `llvm-config --cxxflags --ldflags --system-libs --libs core` -o live-c
