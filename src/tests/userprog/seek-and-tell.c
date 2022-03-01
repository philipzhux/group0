/* Tests writing to standard output. */

#include "tests/lib.h"
#include "tests/main.h"
#include <string.h>

void test_main(void) {
    int fd1 = open("sample.txt");
    int fd2 = open("sample.txt");
    
    if (fd1 < 2) {
        fail("open() returned fd1: %d", fd1);
    }

    if (fd2 < 2) {
        fail("open() returned fd2: %d", fd2);
    }

    int BUFF_SIZE = 15;
    char buff1[BUFF_SIZE + 1];
    char buff2[BUFF_SIZE + 1];
    buff1[BUFF_SIZE] = '\n';
    buff2[BUFF_SIZE] = '\n';

    seek(fd1, 9);
    if (tell(fd1) != 9) {
        msg("tell on fd1 returned %d", tell(fd1));
    }
    int bytes_read = read(fd1, buff1, BUFF_SIZE);
    if (bytes_read != BUFF_SIZE) {
        msg("read() on fd1 only read %d bytes", bytes_read);
    }
    
    bytes_read = read(fd2, buff2, BUFF_SIZE);
    if (bytes_read != BUFF_SIZE) {
        msg("read() on fd2 only read %d bytes", bytes_read);
    }
    char * s = "buff: ";
    write(1, s, strlen(s));
    write(1, buff1, BUFF_SIZE + 1);

    write(1, s, strlen(s));
    write(1, buff2, BUFF_SIZE + 1);

}
