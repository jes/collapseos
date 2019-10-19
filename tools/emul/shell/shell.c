#include <stdint.h>
#include <stdio.h>
#include <termios.h>
#include "../libz80/z80.h"
#include "kernel-bin.h"

/* Collapse OS shell with filesystem
 *
 * On startup, if "cfsin" directory exists, it packs it as a afke block device
 * and loads it in. Upon halting, unpcks the contents of that block device in
 * "cfsout" directory.
 *
 * Memory layout:
 *
 * 0x0000 - 0x3fff: ROM code from shell.asm
 * 0x4000 - 0x4fff: Kernel memory
 * 0x5000 - 0xffff: Userspace
 *
 * I/O Ports:
 *
 * 0 - stdin / stdout
 * 1 - Filesystem blockdev data read/write. Reads and write data to the address
 *     previously selected through port 2
 */

//#define DEBUG
#define MAX_FSDEV_SIZE 0x20000

// in sync with shell.asm
#define RAMSTART 0x4000
#define STDIO_PORT 0x00
#define FS_DATA_PORT 0x01
// Controls what address (24bit) the data port returns. To select an address,
// this port has to be written to 3 times, starting with the MSB.
// Reading this port returns an out-of-bounds indicator. Meaning:
// 0 means addr is within bounds
// 1 means that we're equal to fsdev size (error for reading, ok for writing)
// 2 means more than fsdev size (always invalid)
// 3 means incomplete addr setting
#define FS_ADDR_PORT 0x02

static Z80Context cpu;
static uint8_t mem[0x10000] = {0};
static uint8_t fsdev[MAX_FSDEV_SIZE] = {0};
static uint32_t fsdev_ptr = 0;
// 0 = idle, 1 = received MSB (of 24bit addr), 2 = received middle addr
static int  fsdev_addr_lvl = 0;
static int running;

static uint8_t io_read(int unused, uint16_t addr)
{
    addr &= 0xff;
    if (addr == STDIO_PORT) {
        int c = getchar();
        if (c == EOF) {
            running = 0;
        }
        return (uint8_t)c;
    } else if (addr == FS_DATA_PORT) {
        if (fsdev_addr_lvl != 0) {
            fprintf(stderr, "Reading FSDEV in the middle of an addr op (%d)\n", fsdev_ptr);
            return 0;
        }
        if (fsdev_ptr < MAX_FSDEV_SIZE) {
#ifdef DEBUG
            fprintf(stderr, "Reading FSDEV at offset %d\n", fsdev_ptr);
#endif
            return fsdev[fsdev_ptr];
        } else {
            if (fsdev_ptr >= MAX_FSDEV_SIZE) {
                fprintf(stderr, "Out of bounds FSDEV read at %d\n", fsdev_ptr);
            }
            return 0;
        }
    } else if (addr == FS_ADDR_PORT) {
        if (fsdev_addr_lvl != 0) {
            return 3;
        } else if (fsdev_ptr > MAX_FSDEV_SIZE) {
            return 2;
        } else {
            return 0;
        }
    } else {
        fprintf(stderr, "Out of bounds I/O read: %d\n", addr);
        return 0;
    }
}

static void io_write(int unused, uint16_t addr, uint8_t val)
{
    addr &= 0xff;
    if (addr == STDIO_PORT) {
        if (val == 0x04) { // CTRL+D
            running = 0;
        } else {
            putchar(val);
        }
    } else if (addr == FS_DATA_PORT) {
        if (fsdev_addr_lvl != 0) {
            fprintf(stderr, "Writing to FSDEV in the middle of an addr op (%d)\n", fsdev_ptr);
            return;
        }
        if (fsdev_ptr < MAX_FSDEV_SIZE) {
#ifdef DEBUG
            fprintf(stderr, "Writing to FSDEV (%d)\n", fsdev_ptr);
#endif
            fsdev[fsdev_ptr] = val;
        } else {
            fprintf(stderr, "Out of bounds FSDEV write at %d\n", fsdev_ptr);
        }
    } else if (addr == FS_ADDR_PORT) {
        if (fsdev_addr_lvl == 0) {
            fsdev_ptr = val << 16;
            fsdev_addr_lvl = 1;
        } else if (fsdev_addr_lvl == 1) {
            fsdev_ptr |= val << 8;
            fsdev_addr_lvl = 2;
        } else {
            fsdev_ptr |= val;
            fsdev_addr_lvl = 0;
        }
    } else {
        fprintf(stderr, "Out of bounds I/O write: %d / %d (0x%x)\n", addr, val, val);
    }
}

static uint8_t mem_read(int unused, uint16_t addr)
{
    return mem[addr];
}

static void mem_write(int unused, uint16_t addr, uint8_t val)
{
    if (addr < RAMSTART) {
        fprintf(stderr, "Writing to ROM (%d)!\n", addr);
    }
    mem[addr] = val;
}

int main()
{
    // Setup fs blockdev
    FILE *fp = popen("../cfspack/cfspack cfsin", "r");
    if (fp != NULL) {
        printf("Initializing filesystem\n");
        int i = 0;
        int c;
        while ((c = fgetc(fp)) != EOF && i < MAX_FSDEV_SIZE) {
            fsdev[i++] = c & 0xff;
        }
        if (i == MAX_FSDEV_SIZE) {
            fprintf(stderr, "Filesytem image too large.\n");
            return 1;
        }
        pclose(fp);
    } else {
        printf("Can't initialize filesystem. Leaving blank.\n");
    }

    // Turn echo off: the shell takes care of its own echoing.
    struct termios termInfo;
    if (tcgetattr(0, &termInfo) == -1) {
        printf("Can't setup terminal.\n");
        return 1;
    }
    termInfo.c_lflag &= ~ECHO;
    termInfo.c_lflag &= ~ICANON;
    tcsetattr(0, TCSAFLUSH, &termInfo);


    // initialize memory
    for (int i=0; i<sizeof(KERNEL); i++) {
        mem[i] = KERNEL[i];
    }
    // Run!
    running = 1;
    Z80RESET(&cpu);
    cpu.ioRead = io_read;
    cpu.ioWrite = io_write;
    cpu.memRead = mem_read;
    cpu.memWrite = mem_write;

    while (running && !cpu.halted) {
        Z80Execute(&cpu);
    }

    printf("Done!\n");
    termInfo.c_lflag |= ECHO;
    termInfo.c_lflag |= ICANON;
    tcsetattr(0, TCSAFLUSH, &termInfo);
    return 0;
}
