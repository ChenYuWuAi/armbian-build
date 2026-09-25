/* ampreg: 从任意 Linux/Android root shell 读写 CS35L41 寄存器
 * 用法: ampreg BUS ADDR REG [VAL]   |   ampreg BUS ADDR dump REG1 REG2 ...
 * 协议: 32bit BE 寄存器地址 + 32bit BE 数据, 8 字节 i2c 消息 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
static int fd, addr;
static int rd(unsigned reg, unsigned char *rb){
    unsigned char ra[4]={reg>>24,reg>>16,reg>>8,reg};
    struct i2c_msg m[2]={{.addr=addr,.flags=0,.len=4,.buf=ra},
                         {.addr=addr,.flags=I2C_M_RD,.len=4,.buf=rb}};
    struct i2c_rdwr_ioctl_data d={.msgs=m,.nmsgs=2};
    return ioctl(fd,I2C_RDWR,&d);
}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"用法: %s BUS ADDR REG [VAL] | %s BUS ADDR dump R1 R2 ..\n",argv[0],argv[0]);return 2;}
    int bus=strtol(argv[1],0,0); addr=strtol(argv[2],0,0);
    char p[32]; snprintf(p,sizeof p,"/dev/i2c-%d",bus);
    if((fd=open(p,O_RDWR))<0){perror(p);return 1;}
    if(ioctl(fd,I2C_SLAVE_FORCE,addr)<0){perror("I2C_SLAVE_FORCE");return 1;}
    if(!strcmp(argv[3],"dump")){
        for(int i=4;i<argc;i++){unsigned r=strtoul(argv[i],0,0);unsigned char b[4]={0};
            if(rd(r,b)<0){printf("%#010x ERR\n",r);continue;}
            printf("%#010x = %02x %02x %02x %02x\n",r,b[0],b[1],b[2],b[3]);}
        return 0;}
    unsigned reg=strtoul(argv[3],0,0);
    if(argc>=5){unsigned v=strtoul(argv[4],0,0);
        unsigned char b[8]={reg>>24,reg>>16,reg>>8,reg,v>>24,v>>16,v>>8,v};
        if(write(fd,b,8)!=8){perror("write");return 1;}
        printf("W %#010x <- %#010x\n",reg,v);}
    else{unsigned char b[4]={0};
        if(rd(reg,b)<0){perror("read");return 1;}
        printf("%#010x = %02x %02x %02x %02x\n",reg,b[0],b[1],b[2],b[3]);}
    close(fd);return 0;}
