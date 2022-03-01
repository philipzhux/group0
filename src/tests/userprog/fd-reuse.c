/* Checks if fds are unaccesible to other programs. */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"
#include <string.h>

void test_main(void) {
  char buf[10];
  int fd = wait(exec("fd-reuse-child"));
  if (fd < 0)
    return;
  int status = read(fd, buf, 10);
  if (status != -1)
    fail("read() returned %d", status);
}