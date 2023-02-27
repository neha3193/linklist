
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

void XOR(uint32_t hexdec)
{
    uint32_t crc1,crc;
    crc1=crc=hexdec&0x80000000; 
    crc=hexdec&0x40000000;
    //crc1=crc1^crc;
    crc1=crc1|crc;
    crc=hexdec&0x30000000;
    crc1=crc1|crc;
    crc=hexdec&0x8000000;
    crc1=crc1|crc;
    crc=hexdec&0x6000000;
    crc1=crc1|crc;
    crc=hexdec&0x1000000;
    crc1=crc1|crc;
    crc=hexdec&0xC00000;
    crc1=crc1|crc;
    crc=hexdec&0x200000;
    crc1=crc1|crc;
    crc=hexdec&0x100000;
    crc1=crc1|crc;
    crc=hexdec&0xF0000;
    crc1=crc1|crc;
    crc=hexdec&0xFC00;
    crc1=crc1|crc;
    crc=hexdec&0x400;
    crc1=crc1|crc;
    crc=hexdec&0x380;
    crc1=crc1|crc;
    crc=hexdec&0x40;
    crc1=crc1|crc;
    crc=hexdec&0x7E;
    crc1=crc1|crc;
    crc=hexdec&0x1;
    crc1=crc1|crc;
    printf("\ncrc(0/1)%lx ",  crc);
}

uint32_t crc32(const char *s,size_t n) {
    uint32_t crc=0xFFFFFFFF;
    size_t i,j;
    for(i=0;i<n;i++) {
        char ch=s[i];
        for(j=0;j<8;j++) {
            uint32_t b=(ch^crc)&1;
            crc>>=1;
            if(b) crc=crc^0xEDB88320;
            ch>>=1;
        }
    }

    return ~crc;
}
int main()
{

    char a[]="00B0110001E300000000E0100033E06460669DF9";
    size_t b=160;
    uint32_t crc=crc32(&a,b);
    printf("Hello World =%lx ",  crc);
   
    XOR(a);
    return 0;
}

