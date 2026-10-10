/* Start-up code for `tcc -run` on Nocturne (replaces TinyCC's lib/runmain.c).
   The program runs inside the tcc process with its own copy of libc, so it simply calls main and
   leaves through that libc's exit(), which flushes the program's stdio and ends the process. */
extern void (*__init_array_start[])(int argc, char **argv, char **envp);
extern void (*__init_array_end[])(int argc, char **argv, char **envp);
int main(int argc, char **argv, char **envp);
void exit(int code);
void __libc_init(void);

int _runmain(int argc, char **argv, char **envp) {
    /* JIT links its own libc, including its own TLS initialization flag. */
    __libc_init();
    for (int i = 0; &__init_array_start[i] != __init_array_end; i++) __init_array_start[i](argc, argv, envp);
    exit(main(argc, argv, envp));
    return 0;
}
