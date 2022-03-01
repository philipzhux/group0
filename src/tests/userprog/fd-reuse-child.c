#include <syscall.h>
#include "tests/lib.h"

int main(void) {
  int handle = open("sample.txt");
  if (handle < 2)
    fail("open() returned %d", handle);
  return handle;
}