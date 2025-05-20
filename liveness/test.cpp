#include <stdio.h>

int main() {
  // int n;
  // scanf("%d", &n);
  // printf("%d\n", n);
  // int j = getn();
  int j;
  scanf("%d", &j);

  for (int i = 0; i < 100; ++i) {
    printf("%d\n", i);
    if (i<j) {
      i = (i + j) / 2;
    }
  }
}

// int getn() {
//   int n;
//   scanf("%d", &n);
//   printf("%d\n", n);
//   return n;
// }

// void scann(int i) {
//   int n;
//   scanf("%d", &n);
//   printf("%d\n", i*n);
// }
