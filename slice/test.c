int main(int argc, char* argv[]) {
    int j = 0;
    int p = 0;
    for (int i = 0; i < argc; ++i) {
        j += i;
        p *= i;
    }
    return j;
}