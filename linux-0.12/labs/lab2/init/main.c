/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

#include <string.h>

static char printbuf[1024];

extern char *strcpy();
extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);

static int sprintf(char * str, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i = vsprintf(str, fmt, args);
	va_end(args);
	return i;
}

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define CON_ROWS ((*(unsigned short *)0x9000e) & 0xff)
#define CON_COLS (((*(unsigned short *)0x9000e) & 0xff00) >> 8)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)
#define ORIG_SWAP_DEV (*(unsigned short *)0x901FA)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;
static char term[32];

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL ,NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL, NULL };

struct drive_info { char dummy[32]; } drive_info;

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them 
 * IDT已经设置默认处理函数，但是中断仍然关闭
 * 等到有了必要的设置，再一一开启。
 */
 	ROOT_DEV = ORIG_ROOT_DEV; //ORIG_ROOT_DEV (*(unsigned short *)0x901FC)
 	SWAP_DEV = ORIG_SWAP_DEV;
	sprintf(term, "TERM=con%dx%d", CON_COLS, CON_ROWS);
	envp[1] = term;	
	envp_rc[1] = term;
 	drive_info = DRIVE_INFO;	// 硬盘参数表
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= 0xfffff000;	// 根据内存大小设置 缓存 大小
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
	mem_init(main_memory_start,memory_end);
	trap_init();    // 中断初始化
	blk_dev_init(); // 块设备初始化
	chr_dev_init(); // 字符设备初始化（实际为空）
	tty_init();     // tty设备（串口以及控制台）初始化
	time_init();    // 读取CMOS中的时间值，换算为Linux内使用值
	sched_init();   // 初始化任务0,用于后面切换到R3做准备
	buffer_init(buffer_memory_end);
	hd_init();      // 硬盘初始化
	floppy_init();  // 软盘初始化，设置
	sti();
	move_to_user_mode(); // 切换到R3，任务0的TSS/LDT/PCB等都是代码写死的,即INIT_TASK宏
	if (!fork()) {		/* we count on this going ok */
		init();
	}
	// 任务0在创建任务1之后，则退化为 idle 进程，反复调用pause
	// pause 中会将当前进程设置为中断可唤醒，然后调用schedule进行进程调度
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;)
		__asm__("int $0x80"::"a" (__NR_pause));
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

void init(void)
{
	int pid,i;

	setup((void *) &drive_info); // 挂载根文件系统
	(void) open("/dev/tty1",O_RDWR,0);
	(void) dup(0);
	(void) dup(0);
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) { // 创建2号进程，执行/etc/rc中命令，然后退出
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i))  // 等待2号进程执行/etc/rc完成
			/* nothing */;
	while (1) {  // 进入循环，监控shell进程，一旦退出则重新创建一个shell进程
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty1",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
