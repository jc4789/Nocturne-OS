/* Native packet-owner check: compile with the normal media library and FFmpeg
 * include paths. Including the owner exposes its private packet callback; no
 * alternate decrypt implementation or test-only product bypass is used. */
#include "../user/libc/media_mse.c"
static unsigned checks,failures;
static void check(bool yes,const char *what){checks++;if(!yes){failures++;printf("CENC_FAIL %s\n",what);}}
static const uint8_t test_key[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
static const uint8_t test_iv[16]={0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff};
static const uint8_t plain[32]={0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51};
static const uint8_t cipher[32]={0x87,0x4d,0x61,0x91,0xb6,0x20,0xe3,0x26,0x1b,0xef,0x68,0x64,0x99,0x0d,0xb6,0xce,0x98,0x06,0xf6,0x6b,0x79,0x70,0xfd,0xff,0x86,0x17,0x18,0x7b,0xb9,0xff,0xfd,0xff};
static AVPacket *sample(bool subsamples,bool invalid){
    AVPacket *p=av_packet_alloc();if(!p)return NULL;int count=subsamples?39:32;
    if(av_new_packet(p,count)<0){av_packet_free(&p);return NULL;}
    AVEncryptionInfo *e=av_encryption_info_alloc(subsamples?2:0,16,16);if(!e){av_packet_free(&p);return NULL;}
    e->scheme=0x63656e63u;for(unsigned i=0;i<16;i++)e->key_id[i]=(uint8_t)i;memcpy(e->iv,test_iv,16);
    if(subsamples){memset(p->data,0xa5,count);memcpy(p->data+3,cipher,5);memcpy(p->data+12,cipher+5,27);
        e->subsamples[0]=(AVSubsampleEncryptionInfo){3,5};e->subsamples[1]=(AVSubsampleEncryptionInfo){4,invalid?28:27};
    }else memcpy(p->data,cipher,32);
    size_t size=0;uint8_t *data=av_encryption_info_add_side_data(e,&size);av_encryption_info_free(e);
    if(!data||av_packet_add_side_data(p,AV_PKT_DATA_ENCRYPTION_INFO,data,size)<0){av_free(data);av_packet_free(&p);return NULL;}return p;
}
int main(void){
    uint8_t keys[32];for(unsigned i=0;i<16;i++)keys[i]=(uint8_t)i;memcpy(keys+16,test_key,16);
    nmedia_mse m={0};struct coded_frame f={0};m.cursor=&f;f.packet=sample(false,false);AVPacket *out=av_packet_alloc();
    check(f.packet&&out,"packet allocation");if(!f.packet||!out)return 1;
    check(packet_read(&m,out)==AVERROR(EAGAIN)&&m.cursor==&f&&nmedia_mse_key_waiting(&m),"missing key preserves ciphertext cursor");
    check(nmedia_mse_keys(&m,keys,32)&&!nmedia_mse_key_waiting(&m),"install key snapshot");
    check(packet_read(&m,out)==0&&!memcmp(out->data,plain,32),"NIST full sample decrypt");
    check(!memcmp(f.packet->data,cipher,32),"retained packet remains encrypted");
    check(!av_packet_get_side_data(out,AV_PKT_DATA_ENCRYPTION_INFO,NULL),"decrypted packet has no crypto marker");
    av_packet_unref(out);av_packet_free(&f.packet);f.packet=sample(true,false);m.cursor=&f;
    check(packet_read(&m,out)==0,"subsample decrypt");
    check(out->size==39&&out->data[0]==0xa5&&out->data[8]==0xa5&&!memcmp(out->data+3,plain,5)&&!memcmp(out->data+12,plain+5,27),"clear bytes and CTR partial-block continuity");
    av_packet_unref(out);av_packet_free(&f.packet);f.packet=sample(true,true);m.cursor=&f;
    check(packet_read(&m,out)==AVERROR_INVALIDDATA&&m.cursor==&f,"malformed subsample length rejected");
    check(nmedia_mse_keys(&m,NULL,0),"revoke keys");
    av_packet_free(&f.packet);f.packet=sample(false,false);m.cursor=&f;
    check(packet_read(&m,out)==AVERROR(EAGAIN),"revoked key blocks ciphertext");
    av_packet_free(&out);av_packet_free(&f.packet);printf("media_cenc_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
