#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int n;
    
    if (argc != 2) {
        fprintf(2, "usage: sleep seconds\n");
        exit(1);
    }
    n = atoi(argv[1]);
    if (n < 0) {
        n = 0;
    }
    pause(n);
    exit(0);
}