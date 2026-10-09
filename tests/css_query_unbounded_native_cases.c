static int checks,failures;
static void check(bool ok,const char*name){checks++;printf("%s %s\n",ok?"OK":"FAIL",name);if(!ok)failures++;fflush(NULL);}
static char *nested(const char*inside,size_t depth){size_t n=strlen(inside);char*s=malloc(n+2*depth+1);memset(s,'(',depth);memcpy(s+depth,inside,n);memset(s+depth+n,')',depth);s[n+2*depth]=0;return s;}
static void queries(void){
    char*s=nested("color:red",512);
    check(css_supports_condition(s,strlen(s),false),"supports512 parenthesis continuation");
    s[strlen(s)-1]=0;check(!css_supports_condition(s,strlen(s),false),"supports511 unbalanced is invalid");free(s);
    s=nested("not (color:nonsense)",300);check(css_supports_condition(s,strlen(s),false),"supports300 nested not true");free(s);
    s=nested("not (not (color:red) and)",200);check(!css_supports_condition(s,strlen(s),false),"deep reserved invalid syntax cannot become true under not");free(s);
    s=nested("(color:red) and (width:1px) or (height:1px)",200);check(!css_supports_condition(s,strlen(s),false),"deep supports mixed logical operators invalid");free(s);
    s=nested("not future(unknown)",200);check(css_supports_condition(s,strlen(s),false),"deep general-enclosed negation preserves supports semantics");free(s);
    sbuf fallback={0};for(int i=0;i<512;i++)sb_puts(&fallback,"var(--missing,");sb_puts(&fallback,"red");for(int i=0;i<512;i++)sb_putc(&fallback,')');
    check(css_supports_declaration("color",5,fallback.p,fallback.n),"supports vars512 linear component validation");
    fallback.p[20]='!';check(!css_supports_declaration("color",5,fallback.p,fallback.n),"nested malformed var argument still invalid");sb_free(&fallback);
    size_t large=2*1024*1024;char*long_value=malloc(large+1);memset(long_value,' ',large);memcpy(long_value,"red",3);long_value[large]=0;
    check(css_supports_declaration("color",5,long_value,large),"supports twoMiB value no old65536 or scratch1MiB quota");free(long_value);
    char*name=malloc(1025);memset(name,'a',1024);name[0]=name[1]='-';name[1024]=0;
    check(css_supports_declaration(name,1024,"red",3),"custom property name1024 no arbitrary255 gate");free(name);
    s=nested("width:800px",2048);check(css_media_evaluate(s,800,600,true,NULL,0),"media2048 continuation chain");
    s[strlen(s)-1]=0;check(!css_media_evaluate(s,800,600,true,NULL,0),"media2047 unbalanced remains invalid");free(s);
    s=nested("not (unknown-feature:1)",150);check(!css_media_evaluate(s,800,600,true,NULL,0),"media unknown tri-value cannot become true under not");free(s);
    s=nested("(width:800px) and (height:600px) or (color:8)",150);check(!css_media_evaluate(s,800,600,true,NULL,0),"media deep mixed operator invalid");free(s);
    s=nested("(width:800px) or (height:1px)",150);check(css_media_evaluate(s,800,600,true,NULL,0),"media deep or allowed inside condition");free(s);
    large=70000;s=malloc(large+1);memset(s,' ',large);memcpy(s,"(width:800px)",13);s[large]=0;
    check(css_media_evaluate(s,800,600,true,NULL,0),"media70000 input no old16384 quota");free(s);
    char number[256];memset(number,'0',200);memcpy(number+200,"800",4);char query[280];snprintf(query,sizeof query,"(width:%spx)",number);
    check(css_media_evaluate(query,800,600,true,NULL,0),"media number203 token not old64 truncation");
    fail_calloc=1;check(!css_supports_condition("(color:red)",11,false),"supports actual scratch allocation failure failclosed");
    fail_calloc=1;check(!css_media_evaluate("(width:800px)",800,600,true,NULL,0),"media actual continuation allocation failure failclosed");
    fail_calloc=0;
}
static bool same(const char*a,const char*b){return a&&b&&!strcmp(a,b);}
static void variables(void){
    enum{COUNT=512};web_doc d={0};struct custom_prop*props=calloc(COUNT,sizeof*props);char(*names)[24]=calloc(COUNT,sizeof*names);char(*values)[64]=calloc(COUNT,sizeof*values);
    for(int i=0;i<COUNT;i++){snprintf(names[i],sizeof names[i],"--v%d",i);if(i+1<COUNT)snprintf(values[i],sizeof values[i],"var(--v%d)",i+1);else strcpy(values[i],"7px");props[i]=(struct custom_prop){names[i],values[i],i+1<COUNT?&props[i+1]:NULL};}
    resolve_vars(&d,props,NULL);check(same(props[0].value,"7px")&&same(props[COUNT-1].value,"7px"),"custom graph512 dependency chain resolves past old16");
    check(!d.smem.trap,"custom graph scratch and target traps restored");ar_free(&d.smem);free(props);free(names);free(values);
    struct custom_prop base={"--base","red",NULL},a={"--a","var(--base, var(--a))",&base},b={"--b","var(--a, blue)",&a};
    resolve_vars(&d,&b,&base);check(a.value==NULL,"unused fallback self edge is guaranteed-invalid cycle");check(same(b.value," blue"),"outside cycle uses real fallback");check(same(base.value,"red"),"inherited property immutable and excluded from child cycle graph");ar_free(&d.smem);
    struct custom_prop x={"--x","var(--y)",NULL},y={"--y","var(--x)",&x},z={"--z","var(--x, green)",&y};
    resolve_vars(&d,&z,NULL);check(x.value==NULL&&y.value==NULL,"both mutual SCC members invalid");check(same(z.value," green"),"dependency outside mutual SCC remains valid with fallback");ar_free(&d.smem);
    struct custom_prop leaf={"--leaf","red",NULL},left={"--left","var(--leaf)",&leaf},right={"--right","var(--leaf)",&left},diamond={"--diamond","var(--left) var(--right)",&right};
    resolve_vars(&d,&diamond,NULL);check(same(diamond.value,"red red"),"shared dependency DAG is not false cycle");ar_free(&d.smem);
    struct custom_prop empty={"--empty","",NULL};sbuf out={0};
    check(subst("var(--empty,blue)",17,&empty,&out)&&out.n==0,"valid empty custom value suppresses fallback");sb_free(&out);
    empty.value=NULL;check(subst("var(--empty,blue)",17,&empty,&out)&&same(out.p,"blue"),"guaranteed-invalid differs from empty and selects fallback");sb_free(&out);
    sbuf fallback={0};for(int i=0;i<512;i++)sb_puts(&fallback,"var(--missing,");sb_puts(&fallback,"red");for(int i=0;i<512;i++)sb_putc(&fallback,')');
    check(subst(fallback.p,fallback.n,NULL,&out)&&same(out.p,"red"),"substitution512 fallback stack no C recursion");sb_free(&fallback);sb_free(&out);
    const char*literal="\"var(--missing)\" #var(--missing) @var(--missing)";
    check(subst(literal,strlen(literal),NULL,&out)&&same(out.p,literal),"strings and hash/at keywords not substituted");sb_free(&out);
    check(!subst("var(no,red)",11,NULL,&out),"invalid noncustom var name rejected");sb_free(&out);
    struct custom_prop raw={"--raw","var(--raw)",NULL};check(!subst("var(--raw)",10,&raw,&out),"defensive raw custom cycle terminates");sb_free(&out);
    sb_puts(&out,"saved");char*large=malloc(10001);memset(large,'x',10000);large[10000]=0;fail_realloc=1;
    check(!subst(large,10000,NULL,&out)&&out.n==5&&same(out.p,"saved"),"substitution actual output OOM rolls back prefix atomically");free(large);sb_free(&out);fail_realloc=0;
    struct custom_prop oom={"--oom","var(--missing,red)",NULL};reports=0;fail_calloc=1;resolve_vars(&d,&oom,NULL);
    check(oom.value==NULL && reports==1 && !d.smem.trap,"custom graph actual allocation failure failclosed and diagnosed");fail_calloc=0;ar_free(&d.smem);
}
int main(void){queries();variables();printf("css-query-unbounded-native: %d checks, %d failures\n",checks,failures);fflush(NULL);return failures!=0;}
