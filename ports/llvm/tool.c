/* A tool of the LLVM toolchain: clang, clang++, ld.lld, llvm-ar and the rest
   are one program, llvm (C:\Nocturne\System\llvm), which runs the tool its
   first argument names: "llvm clang++ -c a.cpp". A document has one name, so
   each tool's name is this program: it runs llvm with its own name and
   arguments, settings and handles, and ends with its result. llvm's own name
   comes first, so that clang runs its compiler jobs as llvm, not as this
   program. */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    char llvm[4096];
    if (!find_program("llvm", llvm, sizeof llvm)) {
        fprintf(stderr, "%s: the LLVM toolchain (llvm) is not installed\n", argv[0]);
        return 127;
    }
    const char **arguments = malloc(((size_t)argc + 1) * sizeof *arguments);
    if (!arguments) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 127;
    }
    arguments[0] = llvm;
    for (int i = 0; i < argc; i++) arguments[i + 1] = argv[i];
    int32_t handles[3];
    for (int i = 0; i < 3; i++) handles[i] = n_handle_kind(i) >= 0 ? i : -1;
    uint32_t settings = 0;
    while (n_settings[settings]) settings++;
    struct n_launch launch = {.arguments = arguments, .settings = (const char *const *)n_settings,
                              .handles = handles, .argument_count = (uint32_t)argc + 1, .setting_count = settings,
                              .handle_count = 3};
    n_handle app = n_app_launch(llvm, &launch);
    if (app < 0) {
        fprintf(stderr, "%s: %s: %s\n", argv[0], llvm, n_result_text(app));
        return 127;
    }
    int status = 0;
    n_result r = n_app_result(app, -1, &status);
    return r < 0 ? 127 : status;
}
