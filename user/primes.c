#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int p[2];
  pipe(p);

  int pid = fork();
  if(pid == 0){
    close(p[1]);
    int in_fd = p[0];
    while(1){
      int prime;
      if(read(in_fd, &prime, sizeof(prime)) != sizeof(prime)){
        close(in_fd);
        exit(0);
      }
      printf("prime %d\n", prime);
      int next[2];
      pipe(next);
      int cpid = fork();
      if(cpid == 0){
        close(next[1]);
        close(in_fd);
        in_fd = next[0];
        continue;
      } else {
        close(next[0]);
        int x;
        while(read(in_fd, &x, sizeof(x)) == sizeof(x)){
          if(x % prime != 0){
            write(next[1], &x, sizeof(x));
          }
        }
        close(in_fd);
        close(next[1]);
        wait(0);
        exit(0);
      }
    }
  } else {
    close(p[0]);
    for(int i = 2; i <= 280; i++){
      write(p[1], &i, sizeof(i));
    }
    close(p[1]);
    wait(0);
    exit(0);
  }
}