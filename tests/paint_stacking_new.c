static unsigned checks,failures;
#define CHECK(name,value) do{checks++;if(!(value)){failures++;printf("FAIL %s\n",name);}}while(0)
int main(void){
    style_t base={.display=D_BLOCK,.opacity=1,.position=POS_STATIC,.z_auto=true};
    style_t fixed=base,sticky=base,opacity=base,child=base,sibling=base;
    fixed.position=POS_FIXED;sticky.position=POS_STICKY;opacity.opacity=.5f;opacity.z_auto=false;opacity.z_index=999;
    child.position=POS_ABSOLUTE;child.z_auto=false;child.z_index=100;
    sibling.position=POS_RELATIVE;sibling.z_auto=false;sibling.z_index=1;
    box_t root={.kind=B_BLOCK,.st=&base},a={.kind=B_BLOCK,.st=&fixed,.parent=&root},b={.kind=B_BLOCK,.st=&child,.parent=&a},c={.kind=B_BLOCK,.st=&sibling,.parent=&root};
    root.first=&a;a.next=&c;a.first=&b;
    CHECK("auto z fixed is stacking context",stacking_context(&a));
    pvec layers={0};collect_layers(NULL,&layers,&root);
    CHECK("fixed descendant cannot escape outer sort",layers.n==2&&layers.v[0]==&a&&layers.v[1]==&c);
    layers.n=0;collect_layers(NULL,&layers,&a);CHECK("fixed context retains own positioned child",layers.n==1&&layers.v[0]==&b);
    a.st=&sticky;CHECK("auto z sticky is stacking context and layer",stacking_context(&a)&&paint_box_layer(&a));
    layers.n=0;collect_layers(NULL,&layers,&root);CHECK("sticky descendant cannot escape outer sort",layers.n==2&&layers.v[0]==&a&&layers.v[1]==&c);
    a.st=&opacity;CHECK("normal flow opacity establishes atomic layer",stacking_context(&a)&&paint_box_layer(&a));
    CHECK("normal flow opacity ignores nonapplicable z index",layer_z(&a)==0);
    layers.n=0;collect_layers(NULL,&layers,&root);CHECK("opacity child cannot escape group blending",layers.n==2&&layers.v[0]==&a&&layers.v[1]==&c);
    root.kind=B_FLEX;CHECK("opacity flex item keeps applicable z index",layer_z(&a)==999);
    root.kind=B_BLOCK;opacity.opacity=0;layers.n=0;collect_layers(NULL,&layers,&root);
    CHECK("zero opacity retains descendants inside skipped group",layers.n==2&&layers.v[0]==&a&&layers.v[1]==&c);
    sticky.z_auto=false;sticky.z_index=-5;a.st=&sticky;CHECK("sticky explicit negative z remains applicable",layer_z(&a)==-5);
    free(layers.v);printf("new paint stacking boundaries: %u checks / %u failed\n",checks,failures);return failures?1:0;
}
