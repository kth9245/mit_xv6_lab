#include "kernel/types.h"
#include "user/user.h"
#include "kernel/param.h"

int
main(int argc, char* argv[])
{
    if(argc < 2){
        fprintf(2, "usage: xargs command [args...]\n");
        exit(1);
    }
    
    char *args[MAXARG];
    int num_args = 0;
    for (int i = 1; i < argc && num_args < MAXARG - 1; i ++){
        args[num_args++] = argv[i];
    }

    char lines[512] = {0, };
    int r;
    char c;
    int start_arg = 0;
    int end_arg = 0;
    while(1){
        r = read(0, &c, 1);
        if (r == 0) break;

        if (c == 10){
            args[num_args++] = &lines[start_arg];
            start_arg = end_arg + 1;
        }
        else{
            lines[end_arg] = c;
        }
        end_arg++;
    }

    int pid = fork();
    if (pid == 0){
        exec(args[0], args);
        fprintf(2, "xargs: exec %s failed\n", args[0]);
        exit(1);
    }
    else{
        wait(0);
    }
}