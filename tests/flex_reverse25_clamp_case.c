int main(void){
    web_doc doc={0};box_t other={0};D=&doc;doc.root_box=&other;VH=1080;
    struct scene s;scene(&s,false);s.group_style.height=px(155);s.group_style.max_height=px(100);run(&s);
    check(near(s.group.h,100)&&near(s.items[0].y,80)&&near(s.items[2].y,25),
          "reverse cross-start uses actual clamped definite container height");
    printf("flex-reverse25-clamp: %u checks, %u failures\n",checks,failures);return failures!=0;
}
