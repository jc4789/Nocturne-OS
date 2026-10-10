/* Production native request/worker boundaries with only clock/cookie I/O
   substituted. This is not guest delivery or real-site acceptance. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define ssize_t nocturne_test_ssize_t
#include "../build/html-parser-20261010/html-csp-transport-tested.inc"
int errno;
static unsigned checks,failed,cookie_calls;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL report:%d %s\n",__LINE__,#c);}}while(0)
uint64_t uptime_ms(void){return 10;}
long webcookie_export(webcookie_jar *jar,void *out,size_t cap,int64_t now){(void)jar;(void)out;(void)cap;(void)now;return 0;}
long webcookie_get(webcookie_jar *jar,const struct webcookie_context *c,char *out,size_t cap,int64_t now){(void)jar;(void)c;(void)out;(void)cap;(void)now;cookie_calls++;return 0;}
const char *http_response_headers(const struct http_resp *r){return r->headers_full?r->headers_full:r->headers;}
const char *http_response_status_text(const struct http_resp *r){(void)r;return "Accepted";}
static void done(webnet *net,uint64_t id,uint64_t generation,const struct webnet_response *r,void *opaque){(void)net;(void)id;(void)generation;(void)r;(void)opaque;}
static void clear_job(struct job *j){free(j->non_simple);free(j->outgoing.p);free(j->body);free(j->response_headers);}
int main(void){
    CHECK(webnet_report_fields_valid("POST",WEBNET_CSP_REPORT_HEADERS));
    CHECK(!webnet_report_fields_valid("GET",WEBNET_CSP_REPORT_HEADERS));
    CHECK(!webnet_report_fields_valid("POST","Content-Type: text/plain\r\n"));
    CHECK(!webnet_report_fields_valid("POST","Content-Type: application/csp-report\r\nX-Unsafe: one\r\n"));
    CHECK(webnet_wire_report_flag_valid(WEBNET_REPORT,WEBNET_WIRE_REDIRECT_ERROR,WEBNET_CREDENTIALS_SAME_ORIGIN));
    CHECK(!webnet_wire_report_flag_valid(WEBNET_REPORT,0,WEBNET_CREDENTIALS_SAME_ORIGIN));
    CHECK(!webnet_wire_report_flag_valid(WEBNET_REPORT,WEBNET_WIRE_REDIRECT_ERROR,WEBNET_CREDENTIALS_INCLUDE));
    const uint32_t forbidden[]={WEBNET_WIRE_USER_NAVIGATION,WEBNET_WIRE_FORCE_PREFLIGHT,WEBNET_WIRE_SAME_ORIGIN,WEBNET_WIRE_NO_CORS,WEBNET_WIRE_NO_REFERRER,WEBNET_WIRE_IMAGE_UPGRADE,1u<<WEBNET_WIRE_CACHE_SHIFT};
    for(unsigned i=0;i<sizeof forbidden/sizeof *forbidden;i++)CHECK(!webnet_wire_report_flag_valid(WEBNET_REPORT,WEBNET_WIRE_REDIRECT_ERROR|forbidden[i],WEBNET_CREDENTIALS_SAME_ORIGIN));
    webnet net={0};const char body[]="{\"csp-report\":{}}";
    struct webnet_request q={.kind=WEBNET_REPORT,.generation=9,.url="https://collector.test/report",.origin="https://worker.test/source.js",.method="POST",.headers=WEBNET_CSP_REPORT_HEADERS,.body=body,.body_len=sizeof body-1,.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN,.redirect_error=true};
    uint64_t id=webnet_submit(&net,&q,done,NULL);CHECK(id&&net.queue&&net.queue->priority==3);
    struct webnet_wire_request wire={0};if(net.queue)memcpy(&wire,net.queue->tx,sizeof wire);
    CHECK(wire.kind==WEBNET_REPORT&&wire.credentials==WEBNET_CREDENTIALS_SAME_ORIGIN&&!(wire.user_navigation&WEBNET_WIRE_NO_CORS));
    CHECK(webnet_wire_report_flag_valid(wire.kind,wire.user_navigation,wire.credentials));
    CHECK(wire.origin_len==strlen(q.origin)&&wire.headers_len==strlen(WEBNET_CSP_REPORT_HEADERS)&&wire.body_len==sizeof body-1);
    q.method="GET";CHECK(!webnet_submit(&net,&q,done,NULL));q.method="POST";
    q.headers="Content-Type: application/csp-report\r\nAuthorization: not-authorized\r\n";CHECK(!webnet_submit(&net,&q,done,NULL));q.headers=WEBNET_CSP_REPORT_HEADERS;
    q.credentials=WEBNET_CREDENTIALS_INCLUDE;CHECK(!webnet_submit(&net,&q,done,NULL));q.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN;
    q.redirect_error=false;CHECK(!webnet_submit(&net,&q,done,NULL));q.redirect_error=true;
    q.url="file:///data/report";CHECK(!webnet_submit(&net,&q,done,NULL));q.url="https://collector.test/report";
    q.no_cors=true;CHECK(!webnet_submit(&net,&q,done,NULL));q.no_cors=false;
    while(net.queue){struct request *next=net.queue->next;request_free(net.queue);net.queue=next;}
    struct job report={.wire={.kind=WEBNET_REPORT,.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN,.user_navigation=WEBNET_WIRE_REDIRECT_ERROR,.deadline=1000},.headers=WEBNET_CSP_REPORT_HEADERS,.document="https://worker.test/source.js",.origin="https://worker.test"};
    CHECK(!cors_kind(&report)&&!no_cors(&report)&&validate_headers(&report));
    CHECK(prepare_outgoing(&report,"https://collector.test/report","POST",true)&&!report.hop_cookies&&!cookie_calls);
    CHECK(strstr(report.outgoing.p,"Content-Type: application/csp-report\r\n")&&strstr(report.outgoing.p,"Origin: https://worker.test\r\n"));
    CHECK(prepare_outgoing(&report,"https://worker.test/report","POST",false)&&report.hop_cookies&&cookie_calls==1);
    CHECK(body_cb(&report,"secret response",15)==0&&!report.body&&report.body_len==0);
    struct http_resp response={.status=202,.headers_full="X-Secret: never exposed\r\n"};
    CHECK(response_headers(&report,&response,true)&&!report.response_headers&&report.response_headers_len==0);
    clear_job(&report);
    struct job author={.wire={.kind=WEBNET_FETCH,.credentials=WEBNET_CREDENTIALS_OMIT,.user_navigation=WEBNET_WIRE_NO_CORS},.headers=WEBNET_CSP_REPORT_HEADERS};
    CHECK(!validate_headers(&author)&&strstr(author.error,"safelisted"));clear_job(&author);
    CHECK(sizeof(struct webnet_wire_request)==64&&WEBNET_FETCH==4&&WEBNET_REPORT==5);
    printf("CSP report native boundaries: %u checks / %u failed\n",checks,failed);return failed?1:0;
}
