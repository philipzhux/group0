/* Tests writing to standard output. */

#include "tests/lib.h"
#include "tests/main.h"
#include <string.h>

void test_main(void) {
    int fd = open("sample.txt");
    if (fd < 2) {
        fail("open() returned %d", fd);
    }
    int BUFF_SIZE = 15;
    char buffer[BUFF_SIZE + 1];
    buffer[BUFF_SIZE] = '\n';
    seek(fd, 9);
    if (tell(fd) != 9) {
        msg("tell returned %d", tell(fd));
    }
    int bytes_read = read(fd, buffer, BUFF_SIZE);
    if (bytes_read != BUFF_SIZE) {
        msg("read() only read %d bytes", bytes_read);
    }
    write(1, buffer, BUFF_SIZE + 1);
}
