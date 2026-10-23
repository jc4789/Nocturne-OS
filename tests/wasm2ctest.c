/* 実Nocturneのwasm2cとTCCで生成・コンパイル・実行を確認する。
 * 一時フォルダー（n_temp_folder()）に自分のフォルダーを作って使い、終わると消す。
 * ホストのコンパイラーを経由しない。 */
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
/* The files of the run, by name inside the folder made for it. */
enum {WASM,DRIVER,MODULE,HEADER,PROGRAM,FILES};
static const char *const leaves[FILES]={"wasm2c-smoke.wasm","wasm2c-driver.c","wasm2c-smoke.c","wasm2c-smoke.h","wasm2c-smoke"};
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
    char folder[512],path[FILES][640],include[640];
    int ok=n_path_join(n_temp_folder(),"wasm2c-test",folder,sizeof folder)>0;
    for(int i=0;i<FILES&&ok;i++)ok=n_path_join(folder,leaves[i],path[i],sizeof path[i])>0;
    if(!ok){printf("FAIL wasm2ctest: no path for the files\n");return 1;}
    snprintf(include,sizeof include,"-I%s",folder);
    char *translate[]={"wasm2c","-n","smoke",path[WASM],"-o",path[MODULE],NULL};
    char *compile[]={"tcc",include,"-o",path[PROGRAM],path[DRIVER],path[MODULE],"-lwasm-rt",NULL};
    char *run[]={path[PROGRAM],NULL};
    n_result made=n_folder_create(folder);
    ok=(made==0||made==-N_ERR_EXISTS)&&
        save(path[WASM],binary,sizeof binary)&&
        save(path[DRIVER],driver,strlen(driver))&&
        execute("wasm2c",translate)&&execute("tcc",compile)&&
        execute(run[0],run);
    for(int i=0;i<FILES;i++)n_entry_remove(path[i]);
    n_entry_remove(folder);
    printf("%s wasm2ctest: guest wasm2c -> guest TCC -> native executable\n",ok?"PASS":"FAIL");
    return !ok;
}
