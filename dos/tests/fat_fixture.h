/* Hand-written BPB/directory/FAT: independent of tools/make-fat-image.py. */
#ifndef TEST_FAT_FIXTURE_H
#define TEST_FAT_FIXTURE_H
#include <stdint.h>
#include <string.h>
static void fixture16(uint8_t *p,uint16_t x) { p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void fixture32(uint8_t *p,uint32_t x) { fixture16(p,(uint16_t)x); fixture16(p+2,(uint16_t)(x>>16)); }
static void fixture12(uint8_t *image,uint16_t c,uint16_t x)
{
    for(unsigned i=0;i<2;i++) {
        uint8_t *p=image+512+i*512+c+c/2;
        uint16_t old=p[0]|((uint16_t)p[1]<<8);
        fixture16(p,(c&1) ? (uint16_t)((old&15)|(x<<4)) : (uint16_t)((old&0xF000)|x));
    }
}
static void fixture(uint8_t image[51200])
{
    memset(image,0,51200);
    fixture16(image+11,512); image[13]=1; fixture16(image+14,1);
    image[16]=2; fixture16(image+17,32); fixture16(image+19,100);
    image[21]=0xF0; fixture16(image+22,1); image[510]=0x55; image[511]=0xAA;
    image[512]=image[1024]=0xF0; image[513]=image[514]=image[1025]=image[1026]=0xFF;
    memcpy(image+1536,"HELLO   TXT",11); image[1547]=0x20;
    fixture16(image+1562,2); fixture32(image+1564,19);
    memcpy(image+2560,"W5 FAT says hello\r\n",19); fixture12(image,2,0xFFF);
}
#endif
