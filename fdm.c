// fdm.c - KNC Firmware Debug Monitor driver (path A, unsigned 0x70000 exec). gcc -O2 -o fdm fdm.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>
#define BDF "0000:86:00.0"
#define BR  "0000:85:00.0"
#define R0  "/sys/bus/pci/devices/" BDF "/resource0"
#define R4  "/sys/bus/pci/devices/" BDF "/resource4"
#define SB   0x10000u              /* SBOX base in BAR4 window */
#define DCC00 (SB+0xcc00u)
#define GATE  (SB+0x4118u)
#define POST  0x242cu              /* DBOX postcode, window off 0 */
#define S_DL  (SB+0xab28u)         /* SCRATCH2 dl-ready + apic_id (BAR4) */
#define APIC7 (SB+0xaa08u)
#define MB    0x72000u             /* GDDR mailbox (BAR0) - keeps all SBOX scratch free */
#define M_CMD (MB+0u)
#define M_ALO (MB+4u)
#define M_AHI (MB+8u)
#define M_VLO (MB+0xcu)
#define M_VHI (MB+0x10u)
static const unsigned char MON[] = {250,243,144,187,0,32,7,0,139,19,133,210,116,243,139,123,4,139,115,8,72,193,230,32,72,9,247,131,250,2,116,37,131,250,4,116,39,131,250,3,116,12,131,250,5,116,48,139,15,137,75,12,235,63,72,139,15,137,75,12,72,193,233,32,137,75,16,235,48,139,75,12,137,15,235,41,139,75,12,68,139,67,16,73,193,224,32,76,9,193,72,137,15,235,22,73,137,249,139,123,12,139,115,16,65,255,209,187,0,32,7,0,137,67,12,235,0,187,0,32,7,0,199,3,0,0,0,0,233,124,255,255,255,0,0,0};
static volatile unsigned char *b4; static unsigned char *b0;
static sigjmp_buf jb; static void onflt(int s){ (void)s; siglongjmp(jb,1); }
static uint32_t rd(uint32_t o){ if(sigsetjmp(jb,1)) return 0xffffffffu; return *(volatile uint32_t*)(b4+o); }
static void     wr(uint32_t o,uint32_t v){ if(sigsetjmp(jb,1)) return; *(volatile uint32_t*)(b4+o)=v; }
static uint32_t rb(uint32_t o){ if(sigsetjmp(jb,1)) return 0xffffffffu; return *(volatile uint32_t*)(b0+o); }
static void     wb(uint32_t o,uint32_t v){ if(sigsetjmp(jb,1)) return; *(volatile uint32_t*)(b0+o)=v; }
static int mapbars(void){
  int f4=open(R4,O_RDWR|O_SYNC); if(f4<0){perror("open r4");return -1;}
  b4=mmap(0,0x20000,PROT_READ|PROT_WRITE,MAP_SHARED,f4,0); if(b4==MAP_FAILED){perror("mmap r4");return -1;}
  int f0=open(R0,O_RDWR|O_SYNC); if(f0<0){perror("open r0");return -1;}
  b0=mmap(0,0x100000,PROT_READ|PROT_WRITE,MAP_SHARED,f0,0); if(b0==MAP_FAILED){perror("mmap r0");return -1;}
  return 0;
}
static int parked(void){ uint32_t d=rd(DCC00); return d!=0xffffffffu && d!=0u && (rd(POST)&0xffff)==0x3231u; }
static int mapbars(void);
static int remap(void){ if(b4)munmap((void*)b4,0x20000); if(b0)munmap(b0,0x100000); b4=0;b0=0; return mapbars(); }
static void restore_cfg(void){
  system("DEV=/sys/bus/pci/devices/" BDF ";"
    "AP=$(( $(awk 'NR==1{print $1}' $DEV/resource) )); MM=$(( $(awk 'NR==5{print $1}' $DEV/resource) ));"
    "setpci -s " BDF " 0x10.l=$(printf 0x%08x $((AP&0xffffffff))) 0x14.l=$(printf 0x%08x $((AP>>32)))"
    " 0x20.l=$(printf 0x%08x $((MM&0xffffffff))) 0x24.l=$(printf 0x%08x $((MM>>32))) COMMAND=0x0006 >/dev/null");
}
static int recover(void){
  { uint32_t g=rd(GATE); if(g!=0xffffffffu) wr(GATE,g&0xffffff00u); } /* clear go-gate so fboot PARKS, not jumps to stale 0x70000 */
  system("BC=$(setpci -s " BR " BRIDGE_CONTROL); setpci -s " BR " BRIDGE_CONTROL=$(printf %04x $((0x$BC|0x40)));"
         "sleep 0.3; setpci -s " BR " BRIDGE_CONTROL=$BC");
  /* SBR drops the link -> kernel remove+readd invalidates our mmap; wait for config to come back, then re-mmap */
  for(int i=0;i<40;i++){ if(system("setpci -s " BDF " 0x00.l 2>/dev/null | grep -qi 225c8086")==0) break; sleep(1); }
  restore_cfg();
  if(remap()){ fprintf(stderr,"[recover] remap failed\n"); return -1; }
  /* poll read-only so we don't disturb the booting card; only re-restore if it drops off the bus */
  for(int i=0;i<60;i++){ if(parked()) return 0; if(rd(DCC00)==0xffffffffu){ restore_cfg(); remap(); } sleep(1); }
  fprintf(stderr,"[recover] FAILED DCC00=%08x post=%04x\n",rd(DCC00),rd(POST)&0xffff); return -1;
}
static void inject(void){
  memcpy(b0+0x70000,MON,sizeof(MON));
  wb(M_CMD,0);                 /* clear stale mailbox before the monitor starts polling */
  wr(GATE, rd(GATE)|1u);
  uint32_t apic=(rd(S_DL)>>1)&0x1ffu; if(apic==0) apic=0xf0u;  /* BSP apic_id fallback (SCRATCH2=0 off-park) */
  wr(APIC7+4,apic); wr(APIC7,0x20f2u); usleep(800000);
}
static int mcmd(uint32_t cmd,uint64_t a,uint32_t vlo,uint32_t vhi){
  wb(M_ALO,(uint32_t)a); wb(M_AHI,(uint32_t)(a>>32));
  if(cmd==2||cmd==4||cmd==5){ wb(M_VLO,vlo); wb(M_VHI,vhi); }
  wb(M_CMD,cmd);
  for(int i=0;i<2000;i++){ if(rb(M_CMD)==0) return 0; usleep(500); }
  fprintf(stderr,"[HANG] cmd=%u addr=%llx (monitor stalled; run 'fdm recover')\n",cmd,(unsigned long long)a); return -1;
}
static uint64_t X(const char*s){ return strtoull(s,0,16); }
int main(int argc,char**argv){
  if(argc<2){ fprintf(stderr,"usage: fdm status|recover|up|peek A|poke A V|peek64 A|poke64 A VLO VHI|call A [a0 a1]|dump A N|raw4 OFF [V]\n"); return 2; }
  if(mapbars()) return 1;
  signal(SIGBUS,onflt); signal(SIGSEGV,onflt);
  const char*c=argv[1];
  if(!strcmp(c,"status")){
    printf("DCC00=%08x postcode=%04x SCRATCH2=%08x parked=%d\n",rd(DCC00),rd(POST)&0xffff,rd(S_DL),parked());
  } else if(!strcmp(c,"recover")){ return recover();
  } else if(!strcmp(c,"up") || !strcmp(c,"inject") || !strcmp(c,"fi")){
    if(!strcmp(c,"up")){ if(recover()) return 1; }        /* up: ALWAYS SBR to a clean park first (parked() can't see a running monitor) */
    else if(!strcmp(c,"inject") && !parked()){ fprintf(stderr,"not parked (DCC00=%08x post=%04x); run 'fdm up' or 'fdm fi'\n",rd(DCC00),rd(POST)&0xffff); return 1; }
    /* fi = force-inject regardless of boot state (e.g. texture-SKU boot at postcode 0x3033) */
    inject();
    uint32_t t; if(mcmd(1,0x8007dcc00ull,0,0)){ printf("injected but selftest HANG (monitor not running)\n"); return 1; } t=rb(M_VLO);
    printf("%s: injected. selftest PEEK DCC00=%08x (direct=%08x) %s\n",c,t,rd(DCC00),t==rd(DCC00)?"OK":"MISMATCH");
  } else if(!strcmp(c,"peek")){ if(mcmd(1,X(argv[2]),0,0))return 1; printf("%08x\n",rb(M_VLO));
  } else if(!strcmp(c,"poke")){ if(mcmd(2,X(argv[2]),(uint32_t)X(argv[3]),0))return 1; printf("ok\n");
  } else if(!strcmp(c,"peek64")){ if(mcmd(3,X(argv[2]),0,0))return 1; printf("%08x%08x\n",rb(M_VHI),rb(M_VLO));
  } else if(!strcmp(c,"poke64")){ if(mcmd(4,X(argv[2]),(uint32_t)X(argv[3]),(uint32_t)X(argv[4])))return 1; printf("ok\n");
  } else if(!strcmp(c,"call")){ uint32_t a0=argc>3?(uint32_t)X(argv[3]):0,a1=argc>4?(uint32_t)X(argv[4]):0; if(mcmd(5,X(argv[2]),a0,a1))return 1; printf("ret=%08x\n",rb(M_VLO));
  } else if(!strcmp(c,"dump")){ uint64_t a=X(argv[2]); int n=atoi(argv[3]); for(int i=0;i<n;i++){ if(mcmd(1,a+4*i,0,0)){printf("%09llx HANG\n",(unsigned long long)(a+4*i));return 1;} printf("%09llx: %08x\n",(unsigned long long)(a+4*i),rb(M_VLO)); }
  } else if(!strcmp(c,"raw4")){ if(argc>3){ wr((uint32_t)X(argv[2]),(uint32_t)X(argv[3])); printf("ok\n"); } else printf("%08x\n",rd((uint32_t)X(argv[2])));
  } else { fprintf(stderr,"unknown cmd %s\n",c); return 2; }
  return 0;
}
