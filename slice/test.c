int abc(int d) { return d * 100; }

int main(int argc, char* argv[]) {
    int j = 1;
    int d = abc(10);
    j = abc(j);
    return j;
}