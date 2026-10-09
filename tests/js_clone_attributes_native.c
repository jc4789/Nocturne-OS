/* Focused native QuickJS supplement; not a browser/site substitute. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main
int main(int argc,char **argv){
    (void)argc;(void)argv;
    external_case("js_clone_attribute_cases.js", ";runCloneAttributeCases().then(n=>{console.log('Clone attribute API checks '+n);check('clone-attribute-count',n>=580);mark('api-done');},e=>{console.error(e);mark('api-done');});", BASE);
    printf("js_clone_attributes_native: %d checks, %d failed\n",total,failed);
    return failed!=0;
}
