/* 実Nocturneのwasm2cとTCCで生成・コンパイル・実行を確認する。
 * 専用試験ディスク上の/homeのみを使い、ホストのコンパイラーを経由しない。 */
#include <nocturne.h>
#include <stdio.h>
#include <string.h>

static int save(const char *path,const void *data,size_t size){
    FILE *file=fopen(path,"wb");
    if(!file)return 0;
    int ok=fwrite(data,1,size,file)==size;
    if(fclose(file))ok=0;
    return ok;
}
static int execute(const char *path,char *const argv[]){
    return run_wait(path,argv)==0;
}
int main(void){
    /* (module (func (export "run") (param i32) (result i32)
     *   local.get 0 i32.const 1 i32.add)) */
    static const unsigned char binary[]={
        0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,3,2,1,0,
        7,7,1,3,114,117,110,0,0,10,9,1,7,0,32,0,65,1,106,11
    };
    static const char driver[]=
        "#include <stdio.h>\n#include \"wasm2c-smoke.h\"\n"
        "int main(void){w2c_smoke instance;wasm_rt_init();"
        "wasm2c_smoke_instantiate(&instance);"
        "unsigned result=w2c_smoke_run(&instance,41);"
        "wasm2c_smoke_free(&instance);wasm_rt_free();"
        "printf(\"WASM2C_NATIVE_RESULT %u\\n\",result);return result!=42;}\n";
    char *translate[]={"wasm2c","-n","smoke","/home/wasm2c-smoke.wasm","-o","/home/wasm2c-smoke.c",NULL};
    char *compile[]={"tcc","-I/home","-o","/home/wasm2c-smoke","/home/wasm2c-driver.c","/home/wasm2c-smoke.c","-lwasm-rt",NULL};
    char *run[]={"/home/wasm2c-smoke",NULL};
    int ok=save("/home/wasm2c-smoke.wasm",binary,sizeof binary)&&
        save("/home/wasm2c-driver.c",driver,strlen(driver))&&
        execute("/bin/wasm2c",translate)&&execute("/bin/tcc",compile)&&
        execute(run[0],run);
    printf("%s wasm2ctest: guest wasm2c -> guest TCC -> native executable\n",ok?"PASS":"FAIL");
    return !ok;
}
