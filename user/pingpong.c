#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int parent_p[2];
  int child_p[2];
  char buf[4] = {0, };
  pipe(parent_p);
  pipe(child_p);
  if(fork() == 0){
    // child
    close(parent_p[1]);
    close(child_p[0]);
    read(parent_p[0], buf, 1);
    if (buf[0] != 0) {
        printf("%d: received ping\n", getpid());
    }
    write(child_p[1], "A", 1);
    close(parent_p[0]);
    close(child_p[1]);
    exit(0);
  } else {
    // parent
    close(parent_p[0]);
    close(child_p[1]);
    write(parent_p[1], "A", 1);
    read(child_p[0], buf, 1);
    if (buf[0] != 0) {
        printf("%d: received pong\n", getpid());
    }
    close(parent_p[1]);
    close(child_p[0]);
    wait(0);
  }
  exit(0);
}