#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) { printf("hello from a musl-libc static binary, pid %d, %d args\n", (int)getpid(), argc); return 0; }
