// 8086tiny: a tiny, highly functional, highly portable PC emulator/VM
// Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
//
// Revision 1.25
//
// This work is licensed under the MIT License. See included LICENSE.TXT.

// 2026 Takahiro Yamada / port to 16-bit 8086 real mode (ELKS / gcc-ia16)
// Co-author: Grok Build
// - Guest 1MB address space is file-backed (file1MB.img), not a host array
// - Host buffer for guest data is small
// - int is 16-bit on target: use long/unsigned long for 32-bit values and linear addresses
// - Build with -DNO_GRAPHICS (SDL path left intact but unused)

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

#ifndef NO_GRAPHICS
#include "SDL.h"
#endif

// Emulator system constants
#define IO_PORT_COUNT 0x1000
#define GUEST_RAM_SIZE 0x10FFF0UL
#define REGS_BASE 0xF0000UL
#define REGS_AREA_SIZE 0x40
#define XFER_BUF_SIZE 0x2000
#define VIDEO_RAM_SIZE 0x10000
// Guest RAM file cache
#define MEM_CACHE_SIZE 0x200

// Graphics/timer/keyboard update delays (explained later)
#ifndef GRAPHICS_UPDATE_DELAY
#define GRAPHICS_UPDATE_DELAY 360000
#endif
#define KEYBOARD_TIMER_UPDATE_DELAY 20000

// 16-bit register decodes
#define REG_AX 0
#define REG_CX 1
#define REG_DX 2
#define REG_BX 3
#define REG_SP 4
#define REG_BP 5
#define REG_SI 6
#define REG_DI 7

#define REG_ES 8
#define REG_CS 9
#define REG_SS 10
#define REG_DS 11

#define REG_ZERO 12
#define REG_SCRATCH 13

// 8-bit register decodes
#define REG_AL 0
#define REG_AH 1
#define REG_CL 2
#define REG_CH 3
#define REG_DL 4
#define REG_DH 5
#define REG_BL 6
#define REG_BH 7

// FLAGS register decodes
#define FLAG_CF 40
#define FLAG_PF 41
#define FLAG_AF 42
#define FLAG_ZF 43
#define FLAG_SF 44
#define FLAG_TF 45
#define FLAG_IF 46
#define FLAG_DF 47
#define FLAG_OF 48

// Lookup tables in the BIOS binary
#define TABLE_XLAT_OPCODE 8
#define TABLE_XLAT_SUBFUNCTION 9
#define TABLE_STD_FLAGS 10
#define TABLE_PARITY_FLAG 11
#define TABLE_BASE_INST_SIZE 12
#define TABLE_I_W_SIZE 13
#define TABLE_I_MOD_SIZE 14
#define TABLE_COND_JUMP_DECODE_A 15
#define TABLE_COND_JUMP_DECODE_B 16
#define TABLE_COND_JUMP_DECODE_C 17
#define TABLE_COND_JUMP_DECODE_D 18
#define TABLE_FLAGS_BITFIELDS 19

// Bitfields for TABLE_STD_FLAGS values
#define FLAGS_UPDATE_SZP 1
#define FLAGS_UPDATE_AO_ARITH 2
#define FLAGS_UPDATE_OC_LOGIC 4

// Guest memory file and local register mirror:
// dest_w/src_w: operand scratch
// Local mem[] mirrors guest [REGS_BASE, REGS_BASE+REGS_AREA_SIZE)
int mfd;
unsigned char mem[REGS_AREA_SIZE];
unsigned char mem_opcode[6];
unsigned char xfer_buf[XFER_BUF_SIZE];
unsigned char mem_cache[MEM_CACHE_SIZE];
unsigned long mem_cache_base;
int mem_cache_valid = 0;

// Load MEM_CACHE_SIZE bytes from the file, starting at the miss address.
static void mem_cache_fill(unsigned long addr)
{
	int n;

	mem_cache_base = addr;
	mem_cache_valid = 1;
	if (lseek(mfd, (long)mem_cache_base, SEEK_SET) < 0)
	{
		memset(mem_cache, 0, MEM_CACHE_SIZE);
		return;
	}
	n = read(mfd, mem_cache, MEM_CACHE_SIZE);
	if (n < MEM_CACHE_SIZE)
	{
		if (n < 0)
			n = 0;
		memset(mem_cache + n, 0, (unsigned)(MEM_CACHE_SIZE - n));
	}
}

#define MEM_CACHE_TOUCH(a) do { \
	if (!mem_cache_valid || (a) < mem_cache_base || \
	    (a) >= mem_cache_base + (unsigned long)MEM_CACHE_SIZE) \
		mem_cache_fill(a); \
} while (0)

// Read guest memory (regs area is local; everything else is file-backed + cache)
unsigned short memfr(unsigned long addr, int wa)
{
	unsigned char lo, hi;

	if (addr >= REGS_BASE && addr < REGS_BASE + REGS_AREA_SIZE)
	{
		addr -= REGS_BASE;
		return (unsigned short)(wa ? mem[addr] | ((unsigned short)mem[addr + 1] << 8) : mem[addr]);
	}

	MEM_CACHE_TOUCH(addr);
	lo = mem_cache[(unsigned short)(addr - mem_cache_base)];
	if (!wa)
		return lo;
	MEM_CACHE_TOUCH(addr + 1);
	hi = mem_cache[(unsigned short)(addr + 1 - mem_cache_base)];
#ifdef DEBUG
	printf("addr %lx, lo %d, hi %d\n", addr, lo, hi);
#endif
	return (unsigned short)(lo | ((unsigned short)hi << 8));
}

// Write guest memory (write-through: cache and file)
int memfw(unsigned long addr, unsigned short dat, int wa)
{
	unsigned char buf[2];

	if (addr >= REGS_BASE && addr < REGS_BASE + REGS_AREA_SIZE)
	{
		addr -= REGS_BASE;
		mem[addr] = (unsigned char)dat;
		if (wa)
			mem[addr + 1] = (unsigned char)(dat >> 8);
		return 0;
	}

	buf[0] = (unsigned char)dat;
	buf[1] = (unsigned char)(dat >> 8);
	MEM_CACHE_TOUCH(addr);
	mem_cache[(unsigned short)(addr - mem_cache_base)] = buf[0];
	if (wa)
	{
		MEM_CACHE_TOUCH(addr + 1);
		mem_cache[(unsigned short)(addr + 1 - mem_cache_base)] = buf[1];
	}
	if (lseek(mfd, (long)addr, SEEK_SET) < 0)
		return -1;
	if (write(mfd, buf, wa ? 2 : 1) != (wa ? 2 : 1))
		return -1;
	return 0;
}

// Copy len bytes from guest address into host buffer
void mem_read_block(unsigned long addr, unsigned char *buf, unsigned short len)
{
	int n;

	if (len && (addr >= REGS_BASE + REGS_AREA_SIZE ||
	    addr + (unsigned long)len <= REGS_BASE))
	{
		mem_cache_valid = 0;
		if (lseek(mfd, (long)addr, SEEK_SET) < 0)
		{
			memset(buf, 0, len);
			return;
		}
		n = read(mfd, buf, len);
		if (n < 0)
			n = 0;
		if ((unsigned short)n < len)
			memset(buf + n, 0, (unsigned)(len - n));
		return;
	}
	for (unsigned short i = 0; i < len; i++)
		buf[i] = (unsigned char)memfr(addr + i, 0);
}

// Copy len bytes from host buffer into guest address
void mem_write_block(unsigned long addr, unsigned char *buf, unsigned short len)
{
	if (len && (addr >= REGS_BASE + REGS_AREA_SIZE ||
	    addr + (unsigned long)len <= REGS_BASE))
	{
		mem_cache_valid = 0;
		if (lseek(mfd, (long)addr, SEEK_SET) < 0)
			return;
		write(mfd, buf, len);
		return;
	}
	for (unsigned short i = 0; i < len; i++)
		memfw(addr + i, buf[i], 0);
}

// Helper macros

// Decode mod, r_m and reg fields in instruction
#define DECODE_RM_REG scratch2_uint = 4 * !i_mod, \
					  op_to_addr = rm_addr = i_mod < 3 ? SEGREG(seg_override_en ? seg_override : bios_table_lookup[scratch2_uint + 3][i_rm], bios_table_lookup[scratch2_uint][i_rm], regs16[bios_table_lookup[scratch2_uint + 1][i_rm]] + bios_table_lookup[scratch2_uint + 2][i_rm] * i_data1+) : GET_REG_ADDR(i_rm), \
					  op_from_addr = GET_REG_ADDR(i_reg), \
					  i_d && (scratch_uint = op_from_addr, op_from_addr = rm_addr, op_to_addr = scratch_uint)

// Return memory-mapped register location (guest linear address) for register #reg_id
#define GET_REG_ADDR(reg_id) (REGS_BASE + (unsigned long)(i_w ? 2 * reg_id : 2 * reg_id + reg_id / 4 & 7))

// Returns number of top bit in operand (i.e. 8 for 8-bit operands, 16 for 16-bit operands)
#define TOP_BIT 8*(i_w + 1)

// Opcode execution unit helpers
#define OPCODE ;break; case
#define OPCODE_CHAIN ; case

// [I]MUL/[I]DIV/DAA/DAS/ADC/SBB helpers
// On 16-bit int platforms, widen before multiply/shift to get a 32-bit result
#define MUL_MACRO(op_data_type,out_regs) (set_opcode(0x10), \
										  dest_w = memfr(rm_addr, i_w), \
										  out_regs[i_w + 1] = (unsigned short)((op_result = (long)(op_data_type)(i_w ? dest_w : (unsigned char)dest_w) * (long)(op_data_type)*out_regs) >> 16), \
										  regs16[REG_AX] = (unsigned short)op_result, \
										  set_OF(set_CF(op_result - (op_data_type)op_result)))
#define DIV_MACRO(out_data_type,in_data_type,out_regs) (dest_w = memfr(rm_addr, i_w), \
										  scratch_int = (long)(out_data_type)(i_w ? dest_w : (unsigned char)dest_w)) && \
										  !(scratch2_uint = (unsigned long)((in_data_type)(scratch_uint = ((unsigned long)out_regs[i_w+1] << 16) + regs16[REG_AX]) / scratch_int), scratch2_uint - (unsigned long)(out_data_type)scratch2_uint) ? \
										  out_regs[i_w+1] = (unsigned short)(scratch_uint - scratch_int * (*out_regs = (unsigned short)scratch2_uint)) : pc_interrupt(0)
#define DAA_DAS(op1,op2,mask,min) set_AF((((scratch2_uint = regs8[REG_AL]) & 0x0F) > 9) || regs8[FLAG_AF]) && (op_result = regs8[REG_AL] op1 6, set_CF(regs8[FLAG_CF] || (regs8[REG_AL] op2 scratch2_uint))), \
								  set_CF((((mask & 1 ? scratch2_uint : regs8[REG_AL]) & mask) > min) || regs8[FLAG_CF]) && (op_result = regs8[REG_AL] op1 0x60)
#define ADC_SBB_MACRO(a) OP(a##= regs8[FLAG_CF] +), \
						 set_CF(regs8[FLAG_CF] && (op_result == op_dest) || (a op_result < a(long)op_dest)), \
						 set_AF_OF_arith()

// Execute arithmetic/logic operations; dest_w/src_w are operand scratch
#define R_M_OP(dest,op,src) (i_w ? op_dest = CAST(unsigned short)dest, op_result = CAST(unsigned short)dest op (op_source = CAST(unsigned short)src) \
								 : (op_dest = CAST(unsigned char)dest, op_result = CAST(unsigned char)dest op (op_source = CAST(unsigned char)src)))

#define R_M_OP_S(dest,op,src,saddr) (src = memfr(saddr, i_w), \
					R_M_OP(dest,op,src))

#define R_M_OP_D(dest,op,src,daddr) (dest = memfr(daddr, i_w), \
					R_M_OP(dest,op,src), \
					memfw(daddr, dest, i_w))

#define R_M_OP_D_S(dest,op,src,daddr,saddr) (dest = memfr(daddr, i_w), src = memfr(saddr, i_w), \
						R_M_OP(dest,op,src), \
						memfw(daddr, dest, i_w))

#define MEM_OP(daddr,op,saddr) R_M_OP_D_S(dest_w,op,src_w,daddr,saddr)
#define OP(op) MEM_OP(op_to_addr,op,op_from_addr)

// Increment or decrement a register #reg_id (usually SI or DI), depending on direction flag and operand size (given by i_w)
#define INDEX_INC(reg_id) (regs16[reg_id] -= (2 * regs8[FLAG_DF] - 1)*(i_w + 1))

// Helpers for stack operations
#define R_M_PUSH(a) (i_w = 1, R_M_OP_D(dest_w, =, a, SEGREG(REG_SS, REG_SP, --)))
#define R_M_POP(a) (i_w = 1, regs16[REG_SP] += 2, R_M_OP_S(a, =, src_w, SEGREG(REG_SS, REG_SP, -2+)))
#define R_M_PUSH_MEM(addr) (i_w = 1, R_M_OP_D(dest_w, =, src_w = memfr(addr, 1), SEGREG(REG_SS, REG_SP, --)))
#define R_M_POP_MEM(addr) (i_w = 1, regs16[REG_SP] += 2, memfw(addr, memfr(SEGREG(REG_SS, REG_SP, -2+), 1), 1))

// Convert segment:offset to linear address in emulator memory space (must be 32-bit on 16-bit int hosts)
#define SEGREG(reg_seg,reg_ofs,op) ((unsigned long)regs16[reg_seg] << 4) + (unsigned short)(op regs16[reg_ofs])

// Returns sign bit of an 8-bit or 16-bit operand
#define SIGN_OF(a) (1 & (i_w ? CAST(short)a : a) >> (TOP_BIT - 1))

// Reinterpretation cast
#define CAST(a) *(a*)&

// Keyboard driver for console. This may need changing for UNIX/non-UNIX platforms
#ifdef _WIN32
#define KEYBOARD_DRIVER kbhit() && (memfw(0x4A6, getch(), 0), pc_interrupt(7))
#else
#define KEYBOARD_DRIVER (read(0, xfer_buf, 1) == 1) && (memfw(0x4A6, xfer_buf[0], 0), int8_asap = (xfer_buf[0] == 0x1B), pc_interrupt(7))
#endif

// Keyboard driver for SDL
#ifdef NO_GRAPHICS
#define SDL_KEYBOARD_DRIVER KEYBOARD_DRIVER
#else
#define SDL_KEYBOARD_DRIVER sdl_screen ? SDL_PollEvent(&sdl_event) && (sdl_event.type == SDL_KEYDOWN || sdl_event.type == SDL_KEYUP) && (scratch_uint = sdl_event.key.keysym.unicode, scratch2_uint = sdl_event.key.keysym.mod, CAST(short)mem[0x4A6] = 0x400 + 0x800*!!(scratch2_uint & KMOD_ALT) + 0x1000*!!(scratch2_uint & KMOD_SHIFT) + 0x2000*!!(scratch2_uint & KMOD_CTRL) + 0x4000*(sdl_event.type == SDL_KEYUP) + ((!scratch_uint || scratch_uint > 0x7F) ? sdl_event.key.keysym.sym : scratch_uint), pc_interrupt(7)) : (KEYBOARD_DRIVER)
#endif

// Global variable definitions
unsigned char io_ports[IO_PORT_COUNT], *opcode_stream, *regs8, i_rm, i_w, i_reg, i_mod, i_mod_size, i_d, i_reg4bit, raw_opcode_id, xlat_opcode_id, extra, rep_mode, seg_override_en, rep_override_en, trap_flag, int8_asap, scratch_uchar, io_hi_lo, spkr_en, bios_table_lookup[20][256];
unsigned short *regs16, reg_ip, seg_override, file_index, wave_counter, dest_w, src_w, last_cs;
unsigned long op_source, op_dest, rm_addr, op_to_addr, op_from_addr, i_data0, i_data1, i_data2, scratch_uint, scratch2_uint, inst_counter, set_flags_type, GRAPHICS_X, GRAPHICS_Y, pixel_colors[16], vmem_ctr;
long op_result, scratch_int;
int disk[3];
time_t clock_buf;

#ifndef NO_GRAPHICS
SDL_AudioSpec sdl_audio = {44100, AUDIO_U8, 1, 0, 128};
SDL_Surface *sdl_screen;
SDL_Event sdl_event;
unsigned short vid_addr_lookup[VIDEO_RAM_SIZE], cga_colors[4] = {0 /* Black */, 0x1F1F /* Cyan */, 0xE3E3 /* Magenta */, 0xFFFF /* White */};
unsigned char *vid_mem_base;
#endif

// Helper functions

// Set carry flag
char set_CF(int new_CF)
{
	return regs8[FLAG_CF] = !!new_CF;
}

// Set auxiliary flag
char set_AF(int new_AF)
{
	return regs8[FLAG_AF] = !!new_AF;
}

// Set overflow flag
char set_OF(int new_OF)
{
	return regs8[FLAG_OF] = !!new_OF;
}

// Set auxiliary and overflow flag after arithmetic operations
char set_AF_OF_arith()
{
	set_AF((op_source ^= op_dest ^ (unsigned long)op_result) & 0x10);
	if (op_result == (long)op_dest)
		return set_OF(0);
	else
		return set_OF(1 & (regs8[FLAG_CF] ^ op_source >> (TOP_BIT - 1)));
}

// Assemble and return emulated CPU FLAGS register in scratch_uint
void make_flags()
{
	scratch_uint = 0xF002; // 8086 has reserved and unused flags set to 1
	for (int i = 9; i--;)
		scratch_uint += (unsigned long)regs8[FLAG_CF + i] << bios_table_lookup[TABLE_FLAGS_BITFIELDS][i];
}

// Set emulated CPU FLAGS register from regs8[FLAG_xx] values
void set_flags(int new_flags)
{
	for (int i = 9; i--;)
		regs8[FLAG_CF + i] = !!(1 << bios_table_lookup[TABLE_FLAGS_BITFIELDS][i] & new_flags);
}

// Convert raw opcode to translated opcode index. This condenses a large number of different encodings of similar
// instructions into a much smaller number of distinct functions, which we then execute
void set_opcode(unsigned char opcode)
{
	xlat_opcode_id = bios_table_lookup[TABLE_XLAT_OPCODE][raw_opcode_id = opcode];
	extra = bios_table_lookup[TABLE_XLAT_SUBFUNCTION][opcode];
	i_mod_size = bios_table_lookup[TABLE_I_MOD_SIZE][opcode];
	set_flags_type = bios_table_lookup[TABLE_STD_FLAGS][opcode];
}

// Execute INT #interrupt_num on the emulated machine
char pc_interrupt(unsigned char interrupt_num)
{
#ifdef DEBUG_TRACE
	printf("INT %02x from %04x:%04x\n", interrupt_num, regs16[REG_CS], reg_ip);
	fflush(stdout);
#endif
	set_opcode(0xCD); // Decode like INT

	make_flags();
	R_M_PUSH(scratch_uint);
	R_M_PUSH(regs16[REG_CS]);
	R_M_PUSH(reg_ip);
	MEM_OP(REGS_BASE + 2UL * REG_CS, =, 4UL * interrupt_num + 2);
	R_M_OP_S(reg_ip, =, src_w, 4UL * interrupt_num);

	return regs8[FLAG_TF] = regs8[FLAG_IF] = 0;
}

// AAA and AAS instructions - which_operation is +1 for AAA, and -1 for AAS
int AAA_AAS(char which_operation)
{
	return (regs16[REG_AX] += 262 * which_operation*set_AF(set_CF(((regs8[REG_AL] & 0x0F) > 9) || regs8[FLAG_AF])), regs8[REG_AL] &= 0x0F);
}

#ifndef NO_GRAPHICS
void audio_callback(void *data, unsigned char *stream, int len)
{
	for (int i = 0; i < len; i++)
		stream[i] = (spkr_en == 3) && CAST(unsigned short)mem[0x4AA] ? -((54 * wave_counter++ / CAST(unsigned short)mem[0x4AA]) & 1) : sdl_audio.silence;

	spkr_en = io_ports[0x61] & 3;
}
#endif

// Disk transfer through small host buffer into/out of guest memory
static unsigned short disk_xfer(int do_write, int fd, unsigned long guest_addr, unsigned short len)
{
	unsigned short done = 0;
	unsigned short chunk;
	unsigned short n;

	while (done < len) {
		chunk = len - done;
		if (chunk > XFER_BUF_SIZE)
			chunk = XFER_BUF_SIZE;
		if (do_write)
		{
			mem_read_block(guest_addr + done, xfer_buf, chunk);
			n = (unsigned short)write(fd, xfer_buf, chunk);
		}
		else
		{
			n = (unsigned short)read(fd, xfer_buf, chunk);
			if (n)
				mem_write_block(guest_addr + done, xfer_buf, n);
		}
		if (n != chunk)
			return (unsigned short)(done + n);
		done = (unsigned short)(done + chunk);
	}
	return done;
}

// Emulator entry point
int main(int argc, char **argv)
{
	unsigned long list_off_addr;
	unsigned short table_off;
	unsigned long linear_ip;
#ifndef _WIN32
	int stdin_fl = -1;
#endif

#ifndef NO_GRAPHICS
	// Initialise SDL
	SDL_Init(SDL_INIT_AUDIO);
	sdl_audio.callback = audio_callback;
#ifdef _WIN32
	sdl_audio.samples = 512;
#endif
	SDL_OpenAudio(&sdl_audio, 0);
#endif

#ifndef _WIN32
	/* do not block the CPU loop on stdin */
	stdin_fl = fcntl(0, F_GETFL, 0);
	if (stdin_fl >= 0)
		fcntl(0, F_SETFL, stdin_fl | O_NONBLOCK);
#endif

	// Open (or create) guest memory backing file (~1MB+)
	mfd = open("file1mb.img", O_RDWR);
	if (mfd < 0)
	{
		unsigned char z = 0;
		printf("Creating guest memory 1MB file\n");
		mfd = open("file1mb.img", O_RDWR | O_CREAT, 0644);
		if (mfd < 0)
			return -1;
		if (lseek(mfd, (long)GUEST_RAM_SIZE - 1, SEEK_SET) < 0 || write(mfd, &z, 1) != 1)
		{
			close(mfd);
			return -1;
		}
	}

	// regs16 and reg8 point to the local mirror of F000:0. CS is initialised to F000
	memset(mem, 0, sizeof(mem));
	regs16 = (unsigned short *)(regs8 = mem);
	regs16[REG_CS] = 0xF000;

	// Trap flag off
	regs8[FLAG_TF] = 0;

	// Set DL equal to the boot device: 0 for the FD, or 0x80 for the HD. Normally, boot from the FD.
	// But, if the HD image file is prefixed with @, then boot from the HD
	regs8[REG_DL] = ((argc > 3) && (*argv[3] == '@')) ? argv[3]++, 0x80 : 0;

	// Open BIOS (file id disk[2]), floppy disk image (disk[1]), and hard disk image (disk[0]) if specified
	// 32898 == O_RDWR|O_BINARY on the original Win32/Linux builds; use O_RDWR on ELKS/POSIX
	for (file_index = 3; file_index;)
		disk[--file_index] = *++argv ? open(*argv, O_RDWR) : 0;

	// Set CX:AX equal to the hard disk image size, if present (32-bit sector count)
	if (*disk)
	{
		unsigned long sectors = (unsigned long)lseek(*disk, 0, SEEK_END) >> 9;
		regs16[REG_AX] = (unsigned short)sectors;
		regs16[REG_CX] = (unsigned short)(sectors >> 16);
	}
	else
	{
		regs16[REG_AX] = 0;
		regs16[REG_CX] = 0;
	}

	// Load BIOS image into F000:0100, and set IP to 0100
	lseek(disk[2], 0, SEEK_SET);
	lseek(mfd, (long)0xF0100UL, SEEK_SET);
	for (unsigned long off = 0; off < 0xFF00UL; off++)
	{
		unsigned char b;
		if (read(disk[2], &b, 1) != 1)
			break;
		if (write(mfd, &b, 1) != 1)
			break;
		printf("Load BIOS %ld\r", off);
	}
	printf("\n");
	mem_cache_valid = 0;
	reg_ip = 0x100;

	// Load instruction decoding helper tables from BIOS (offsets at F000:0102 == regs16[0x81..])
	for (int i = 0; i < 20; i++)
	{
		list_off_addr = 2UL * (0x81UL + (unsigned long)i);
		table_off = memfr(REGS_BASE + list_off_addr, 1);
#ifdef DEBUG
		printf("list_off_addr : %lx\n", list_off_addr);
		printf("table_off : %x\n", table_off);
#endif
		for (int j = 0; j < 256; j++)
		{
			bios_table_lookup[i][j] = (unsigned char)memfr(REGS_BASE + table_off + (unsigned long)j, 0);
#ifdef DEBUG
			printf("bios_table_lookup : %d %d %d\n", i, j, bios_table_lookup[i][j]);
#endif
		}
	}

	// Instruction execution loop. Terminates if CS:IP = 0:0
	for (;;)
	{
		linear_ip = ((unsigned long)regs16[REG_CS] << 4) + reg_ip;
		if (linear_ip == 0)
			break;

		// Prefetch up to 6 instruction bytes into a local buffer (real-mode safe)
		mem_opcode[0] = (unsigned char)memfr(linear_ip + 0, 0);
		mem_opcode[1] = (unsigned char)memfr(linear_ip + 1, 0);
		mem_opcode[2] = (unsigned char)memfr(linear_ip + 2, 0);
		mem_opcode[3] = (unsigned char)memfr(linear_ip + 3, 0);
		mem_opcode[4] = (unsigned char)memfr(linear_ip + 4, 0);
		mem_opcode[5] = (unsigned char)memfr(linear_ip + 5, 0);
		opcode_stream = mem_opcode;

		// Set up variables to prepare for decoding an opcode
		set_opcode(*opcode_stream);

		// Extract i_w and i_d fields from instruction
		i_w = (i_reg4bit = raw_opcode_id & 7) & 1;
		i_d = i_reg4bit / 2 & 1;

		// Extract instruction data fields
		i_data0 = CAST(short)opcode_stream[1];
		i_data1 = CAST(short)opcode_stream[2];
		i_data2 = CAST(short)opcode_stream[3];

#ifdef DEBUG
		printf("mem_opcode:%x %x %x %x %x %x\n", mem_opcode[0], mem_opcode[1], mem_opcode[2], mem_opcode[3], mem_opcode[4], mem_opcode[5]);
		printf("IP:%x %x %lx Data:%lx %lx %lx\n", reg_ip, regs16[REG_CS], 16 * (unsigned long)regs16[REG_CS] + (unsigned long)reg_ip, i_data0, i_data1, i_data2);
#endif

		// seg_override_en and rep_override_en contain number of instructions to hold segment override and REP prefix respectively
		if (seg_override_en)
			seg_override_en--;
		if (rep_override_en)
			rep_override_en--;

		// i_mod_size > 0 indicates that opcode uses i_mod/i_rm/i_reg, so decode them
		if (i_mod_size)
		{
			i_mod = (i_data0 & 0xFF) >> 6;
			i_rm = i_data0 & 7;
			i_reg = i_data0 / 8 & 7;

			if ((!i_mod && i_rm == 6) || (i_mod == 2))
				i_data2 = CAST(short)opcode_stream[4];
			else if (i_mod != 1)
				i_data2 = i_data1;
			else // If i_mod is 1, operand is (usually) 8 bits rather than 16 bits
				i_data1 = (char)i_data1;

			DECODE_RM_REG;
		}

		// Instruction execution unit
		switch (xlat_opcode_id)
		{
			OPCODE_CHAIN 0: // Conditional jump (JAE, JNAE, etc.)
				// i_w is the invert flag, e.g. i_w == 1 means JNAE, whereas i_w == 0 means JAE
				scratch_uchar = raw_opcode_id / 2 & 7;
				reg_ip += (char)i_data0 * (i_w ^ (regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_A][scratch_uchar]] || regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_B][scratch_uchar]] || regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_C][scratch_uchar]] ^ regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_D][scratch_uchar]]))
			OPCODE 1: // MOV reg, imm
				i_w = !!(raw_opcode_id & 8);
				R_M_OP_D(dest_w, =, i_data0, GET_REG_ADDR(i_reg4bit))
			OPCODE 3: // PUSH regs16
				R_M_PUSH(regs16[i_reg4bit])
			OPCODE 4: // POP regs16
				R_M_POP(regs16[i_reg4bit])
			OPCODE 2: // INC|DEC regs16
				i_w = 1;
				i_d = 0;
				i_reg = i_reg4bit;
				DECODE_RM_REG;
				i_reg = extra
			OPCODE_CHAIN 5: // INC|DEC|JMP|CALL|PUSH
				if (i_reg < 2) // INC|DEC
					MEM_OP(op_from_addr, += 1 - 2 * i_reg +, REGS_BASE + 2UL * REG_ZERO),
					op_source = 1,
					set_AF_OF_arith(),
					set_OF(op_dest + 1 - i_reg == 1UL << (TOP_BIT - 1)),
					(xlat_opcode_id == 5) && (set_opcode(0x10), 0); // Decode like ADC
				else if (i_reg != 6) // JMP|CALL
					i_reg - 3 || R_M_PUSH(regs16[REG_CS]), // CALL (far)
					i_reg & 2 && R_M_PUSH(reg_ip + 2 + i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6)), // CALL (near or far)
					i_reg & 1 && (regs16[REG_CS] = memfr(op_from_addr + 2, 1)), // JMP|CALL (far)
					R_M_OP_S(reg_ip, =, src_w, op_from_addr),
					set_opcode(0x9A); // Decode like CALL
				else // PUSH
					R_M_PUSH_MEM(rm_addr)
			OPCODE 6: // TEST r/m, imm16 / NOT|NEG|MUL|IMUL|DIV|IDIV reg
				op_to_addr = op_from_addr;

				switch (i_reg)
				{
					OPCODE_CHAIN 0: // TEST
						set_opcode(0x20); // Decode like AND
						reg_ip += i_w + 1;
						R_M_OP_D(dest_w, &, i_data2, op_to_addr)
					OPCODE 2: // NOT
						OP(=~)
					OPCODE 3: // NEG
						OP(=-);
						op_dest = 0;
						set_opcode(0x28); // Decode like SUB
						set_CF(op_result > op_dest)
					OPCODE 4: // MUL
						i_w ? MUL_MACRO(unsigned short, regs16) : MUL_MACRO(unsigned char, regs8)
					OPCODE 5: // IMUL
						i_w ? MUL_MACRO(short, regs16) : MUL_MACRO(char, regs8)
					OPCODE 6: // DIV
						i_w ? DIV_MACRO(unsigned short, unsigned long, regs16) : DIV_MACRO(unsigned char, unsigned short, regs8)
					OPCODE 7: // IDIV
						i_w ? DIV_MACRO(short, long, regs16) : DIV_MACRO(char, short, regs8);
				}
			OPCODE 7: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP AL/AX, immed
				rm_addr = REGS_BASE;
				i_data2 = i_data0;
				i_mod = 3;
				i_reg = extra;
				reg_ip--;
			OPCODE_CHAIN 8: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP reg, immed
				op_to_addr = rm_addr;
				regs16[REG_SCRATCH] = (i_d |= !i_w) ? (char)i_data2 : i_data2;
				op_from_addr = REGS_BASE + 2UL * REG_SCRATCH;
				reg_ip += !i_d + 1;
				set_opcode(0x08 * (extra = i_reg));
			OPCODE_CHAIN 9: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP|MOV reg, r/m
				switch (extra)
				{
					OPCODE_CHAIN 0: // ADD
						OP(+=),
						set_CF(op_result < op_dest)
					OPCODE 1: // OR
						OP(|=)
					OPCODE 2: // ADC
						ADC_SBB_MACRO(+)
					OPCODE 3: // SBB
						ADC_SBB_MACRO(-)
					OPCODE 4: // AND
						OP(&=)
					OPCODE 5: // SUB
						OP(-=),
						set_CF(op_result > op_dest)
					OPCODE 6: // XOR
						OP(^=)
					OPCODE 7: // CMP
						OP(-),
						set_CF(op_result > op_dest)
					OPCODE 8: // MOV
						OP(=);
				}
			OPCODE 10: // MOV sreg, r/m | POP r/m | LEA reg, r/m
				if (!i_w) // MOV
					i_w = 1,
					i_reg += 8,
					DECODE_RM_REG,
					OP(=);
				else if (!i_d) // LEA
					seg_override_en = 1,
					seg_override = REG_ZERO,
					DECODE_RM_REG,
					R_M_OP_D(dest_w, =, rm_addr, op_from_addr);
				else // POP
					R_M_POP_MEM(rm_addr)
			OPCODE 11: // MOV AL/AX, [loc]
				i_mod = i_reg = 0;
				i_rm = 6;
				i_data1 = i_data0;
				DECODE_RM_REG;
				MEM_OP(op_from_addr, =, op_to_addr)
			OPCODE 12: // ROL|ROR|RCL|RCR|SHL|SHR|???|SAR reg/mem, 1/CL/imm (80186)
				dest_w = memfr(rm_addr, i_w),
				scratch2_uint = SIGN_OF(dest_w),
				scratch_uint = extra ? // xxx reg/mem, imm
					++reg_ip,
					(char)i_data1
				: // xxx reg/mem, CL
					i_d
						? 31 & regs8[REG_CL]
				: // xxx reg/mem, 1
					1;
				if (scratch_uint)
				{
					if (i_reg < 4) // Rotate operations
						scratch_uint %= i_reg / 2 + TOP_BIT,
						R_M_OP_S(scratch2_uint, =, src_w, rm_addr);
					if (i_reg & 1) // Rotate/shift right operations
						R_M_OP_D(dest_w, >>=, scratch_uint, rm_addr);
					else // Rotate/shift left operations
						R_M_OP_D(dest_w, <<=, scratch_uint, rm_addr);
					if (i_reg > 3) // Shift operations
						set_opcode(0x10); // Decode like ADC
					if (i_reg > 4) // SHR or SAR
						set_CF(op_dest >> (scratch_uint - 1) & 1);
				}

				switch (i_reg)
				{
					OPCODE_CHAIN 0: // ROL
						R_M_OP_D(dest_w, += , scratch2_uint >> (TOP_BIT - scratch_uint), rm_addr);
						set_OF(SIGN_OF(op_result) ^ set_CF(op_result & 1))
					OPCODE 1: // ROR
						scratch2_uint &= (1UL << scratch_uint) - 1,
						R_M_OP_D(dest_w, += , scratch2_uint << (TOP_BIT - scratch_uint), rm_addr);
						set_OF(SIGN_OF(op_result * 2) ^ set_CF(SIGN_OF(op_result)))
					OPCODE 2: // RCL
						R_M_OP_D(dest_w, += (regs8[FLAG_CF] << (scratch_uint - 1)) + , scratch2_uint >> (1 + TOP_BIT - scratch_uint), rm_addr);
						set_OF(SIGN_OF(op_result) ^ set_CF(scratch2_uint & 1UL << (TOP_BIT - scratch_uint)))
					OPCODE 3: // RCR
						R_M_OP_D(dest_w, += (regs8[FLAG_CF] << (TOP_BIT - scratch_uint)) + , scratch2_uint << (1 + TOP_BIT - scratch_uint), rm_addr);
						set_CF(scratch2_uint & 1UL << (scratch_uint - 1));
						set_OF(SIGN_OF(op_result) ^ SIGN_OF(op_result * 2))
					OPCODE 4: // SHL
						set_OF(SIGN_OF(op_result) ^ set_CF(SIGN_OF(op_dest << (scratch_uint - 1))))
					OPCODE 5: // SHR
						set_OF(SIGN_OF(op_dest))
					OPCODE 7: // SAR
						scratch_uint < TOP_BIT || set_CF(scratch2_uint);
						set_OF(0);
						R_M_OP_D(dest_w, +=, scratch2_uint *= ~(((1UL << TOP_BIT) - 1) >> scratch_uint), rm_addr);
				}
			OPCODE 13: // LOOPxx|JCZX
				scratch_uint = !!--regs16[REG_CX];

				switch(i_reg4bit)
				{
					OPCODE_CHAIN 0: // LOOPNZ
						scratch_uint &= !regs8[FLAG_ZF]
					OPCODE 1: // LOOPZ
						scratch_uint &= regs8[FLAG_ZF]
					OPCODE 3: // JCXXZ
						scratch_uint = !++regs16[REG_CX];
				}
				reg_ip += scratch_uint*(char)i_data0
			OPCODE 14: // JMP | CALL short/near
				reg_ip += 3 - i_d;
				if (!i_w)
				{
					if (i_d) // JMP far
						reg_ip = 0,
						regs16[REG_CS] = i_data2;
					else // CALL
						R_M_PUSH(reg_ip);
				}
				reg_ip += i_d && i_w ? (char)i_data0 : i_data0
			OPCODE 15: // TEST reg, r/m
				MEM_OP(op_from_addr, &, op_to_addr)
			OPCODE 16: // XCHG AX, regs16
				i_w = 1;
				op_to_addr = REGS_BASE;
				op_from_addr = GET_REG_ADDR(i_reg4bit);
			OPCODE_CHAIN 24: // NOP|XCHG reg, r/m
				if (op_to_addr != op_from_addr)
					OP(^=),
					MEM_OP(op_from_addr, ^=, op_to_addr),
					OP(^=)
			OPCODE 17: // MOVSx (extra=0)|STOSx (extra=1)|LODSx (extra=2)
				scratch2_uint = seg_override_en ? seg_override : REG_DS;

				for (scratch_uint = rep_override_en ? regs16[REG_CX] : 1; scratch_uint; scratch_uint--)
				{
					MEM_OP(extra < 2 ? SEGREG(REG_ES, REG_DI,) : REGS_BASE, =, extra & 1 ? REGS_BASE : SEGREG(scratch2_uint, REG_SI,)),
					extra & 1 || INDEX_INC(REG_SI),
					extra & 2 || INDEX_INC(REG_DI);
				}

				if (rep_override_en)
					regs16[REG_CX] = 0
			OPCODE 18: // CMPSx (extra=0)|SCASx (extra=1)
				scratch2_uint = seg_override_en ? seg_override : REG_DS;

				if ((scratch_uint = rep_override_en ? regs16[REG_CX] : 1))
				{
					for (; scratch_uint; rep_override_en || scratch_uint--)
					{
						MEM_OP(extra ? REGS_BASE : SEGREG(scratch2_uint, REG_SI,), -, SEGREG(REG_ES, REG_DI,)),
						extra || INDEX_INC(REG_SI),
						INDEX_INC(REG_DI), rep_override_en && !(--regs16[REG_CX] && (!op_result == rep_mode)) && (scratch_uint = 0);
					}

					set_flags_type = FLAGS_UPDATE_SZP | FLAGS_UPDATE_AO_ARITH; // Funge to set SZP/AO flags
					set_CF(op_result > op_dest);
				}
			OPCODE 19: // RET|RETF|IRET
				i_d = i_w;
				R_M_POP(reg_ip);
				if (extra) // IRET|RETF|RETF imm16
					R_M_POP(regs16[REG_CS]);
				if (extra & 2) // IRET
					set_flags(R_M_POP(scratch_uint));
				else if (!i_d) // RET|RETF imm16
					regs16[REG_SP] += i_data0
			OPCODE 20: // MOV r/m, immed
				R_M_OP_D(dest_w, =, i_data2, op_from_addr)
			OPCODE 21: // IN AL/AX, DX/imm8
				io_ports[0x20] = 0; // PIC EOI
				io_ports[0x42] = --io_ports[0x40]; // PIT channel 0/2 read placeholder
				io_ports[0x3DA] ^= 9; // CGA refresh
				scratch_uint = extra ? regs16[REG_DX] : (unsigned char)i_data0;
				scratch_uint == 0x60 && (io_ports[0x64] = 0); // Scancode read flag
				scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 7) && (io_ports[0x3D5] = ((memfr(0x49E, 0)*80 + memfr(0x49D, 0) + memfr(0x4AD, 1)) & (io_ports[0x3D4] & 1 ? 0xFF : 0xFF00)) >> (io_ports[0x3D4] & 1 ? 0 : 8)); // CRT cursor position
				R_M_OP(regs8[REG_AL], =, io_ports[scratch_uint & (IO_PORT_COUNT - 1)]);
			OPCODE 22: // OUT DX/imm8, AL/AX
				scratch_uint = extra ? regs16[REG_DX] : (unsigned char)i_data0;
				R_M_OP(io_ports[scratch_uint & (IO_PORT_COUNT - 1)], =, regs8[REG_AL]);
				scratch_uint == 0x61 && (io_hi_lo = 0, spkr_en |= regs8[REG_AL] & 3); // Speaker control
				(scratch_uint == 0x40 || scratch_uint == 0x42) && (io_ports[0x43] & 6) && (memfw(0x469UL + scratch_uint - (io_hi_lo ^= 1), regs8[REG_AL], 0)); // PIT rate programming
#ifndef NO_GRAPHICS
				scratch_uint == 0x43 && (io_hi_lo = 0, regs8[REG_AL] >> 6 == 2) && (SDL_PauseAudio((regs8[REG_AL] & 0xF7) != 0xB6), 0); // Speaker enable
#endif
				scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 6) && (memfw(0x4ADUL + !(io_ports[0x3D4] & 1), regs8[REG_AL], 0)); // CRT video RAM start offset
				scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 7) && (scratch2_uint = ((memfr(0x49E, 0)*80 + memfr(0x49D, 0) + memfr(0x4AD, 1)) & (io_ports[0x3D4] & 1 ? 0xFF00 : 0xFF)) + (regs8[REG_AL] << (io_ports[0x3D4] & 1 ? 0 : 8)) - memfr(0x4AD, 1), memfw(0x49D, scratch2_uint % 80, 0), memfw(0x49E, scratch2_uint / 80, 0)); // CRT cursor position
				scratch_uint == 0x3B5 && io_ports[0x3B4] == 1 && (GRAPHICS_X = regs8[REG_AL] * 16); // Hercules resolution reprogramming. Defaults are set in the BIOS
				scratch_uint == 0x3B5 && io_ports[0x3B4] == 6 && (GRAPHICS_Y = regs8[REG_AL] * 4);
			OPCODE 23: // REPxx
				rep_override_en = 2;
				rep_mode = i_w;
				seg_override_en && seg_override_en++
			OPCODE 25: // PUSH reg
				R_M_PUSH(regs16[extra])
			OPCODE 26: // POP reg
				R_M_POP(regs16[extra])
			OPCODE 27: // xS: segment overrides
				seg_override_en = 2;
				seg_override = extra;
				rep_override_en && rep_override_en++
			OPCODE 28: // DAA/DAS
				i_w = 0;
				extra ? DAA_DAS(-=, >=, 0xFF, 0x99) : DAA_DAS(+=, <, 0xF0, 0x90) // extra = 0 for DAA, 1 for DAS
			OPCODE 29: // AAA/AAS
				op_result = AAA_AAS(extra - 1)
			OPCODE 30: // CBW
				regs8[REG_AH] = -SIGN_OF(regs8[REG_AL])
			OPCODE 31: // CWD
				regs16[REG_DX] = -SIGN_OF(regs16[REG_AX])
			OPCODE 32: // CALL FAR imm16:imm16
				R_M_PUSH(regs16[REG_CS]);
				R_M_PUSH(reg_ip + 5);
				regs16[REG_CS] = i_data2;
				reg_ip = i_data0
			OPCODE 33: // PUSHF
				make_flags();
				R_M_PUSH(scratch_uint)
			OPCODE 34: // POPF
				set_flags(R_M_POP(scratch_uint))
			OPCODE 35: // SAHF
				make_flags();
				set_flags((scratch_uint & 0xFF00) + regs8[REG_AH])
			OPCODE 36: // LAHF
				make_flags(),
				regs8[REG_AH] = scratch_uint
			OPCODE 37: // LES|LDS reg, r/m
				i_w = i_d = 1;
				DECODE_RM_REG;
				OP(=);
				MEM_OP(REGS_BASE + extra, =, rm_addr + 2)
			OPCODE 38: // INT 3
				++reg_ip;
				pc_interrupt(3)
			OPCODE 39: // INT imm8
				reg_ip += 2;
				pc_interrupt(i_data0)
			OPCODE 40: // INTO
				++reg_ip;
				regs8[FLAG_OF] && pc_interrupt(4)
			OPCODE 41: // AAM
				if (i_data0 &= 0xFF)
					regs8[REG_AH] = regs8[REG_AL] / i_data0,
					op_result = regs8[REG_AL] %= i_data0;
				else // Divide by zero
					pc_interrupt(0)
			OPCODE 42: // AAD
				i_w = 0;
				regs16[REG_AX] = op_result = 0xFF & regs8[REG_AL] + i_data0 * regs8[REG_AH]
			OPCODE 43: // SALC
				regs8[REG_AL] = -regs8[FLAG_CF]
			OPCODE 44: // XLAT
				regs8[REG_AL] = (unsigned char)memfr(SEGREG(seg_override_en ? seg_override : REG_DS, REG_BX, regs8[REG_AL] +), 0)
			OPCODE 45: // CMC
				regs8[FLAG_CF] ^= 1
			OPCODE 46: // CLC|STC|CLI|STI|CLD|STD
				regs8[extra / 2] = extra & 1
			OPCODE 47: // TEST AL/AX, immed
				R_M_OP(regs8[REG_AL], &, i_data0)
			OPCODE 48: // Emulator-specific 0F xx opcodes
				switch ((char)i_data0)
				{
					OPCODE_CHAIN 0: // PUTCHAR_AL
#ifdef DEBUG_TRACE
						printf("CHAR %02x\n", regs8[REG_AL]);
						fflush(stdout);
#endif
						write(1, regs8, 1)
					OPCODE 1: // GET_RTC
						time(&clock_buf);
						{
							struct tm *tm_ptr = localtime(&clock_buf);
							if (tm_ptr)
								mem_write_block(SEGREG(REG_ES, REG_BX,), (unsigned char *)tm_ptr, (unsigned short)sizeof(struct tm));
							/* milliseconds not available on ELKS; leave as zero */
							memfw(SEGREG(REG_ES, REG_BX, 36+), 0, 1);
						}
					OPCODE 2: // DISK_READ
					OPCODE_CHAIN 3: // DISK_WRITE
						{
							unsigned long daddr = SEGREG(REG_ES, REG_BX,);
							unsigned short nbytes = regs16[REG_AX];
							unsigned short sector = regs16[REG_BP];
							int do_wr = ((char)i_data0 == 3);
							int dsk = disk[regs8[REG_DL]];
							unsigned short got = ~lseek(dsk, (long)((unsigned long)sector << 9), SEEK_SET)
								? disk_xfer(do_wr, dsk, daddr, nbytes)
								: 0;
							regs8[REG_AL] = (unsigned char)got;
#ifdef DEBUG_DISK
							printf("DISK %s dl=%x fd=%d sec=%u bytes=%u dest=0x%lx al=%u\n",
								do_wr ? "WR" : "RD", regs8[REG_DL], dsk,
								sector, nbytes, daddr, got);
							fflush(stdout);
#endif
						}
				}
		}

		// Increment instruction pointer by computed instruction length. Tables in the BIOS binary
		// help us here.
		reg_ip += (i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6))*i_mod_size + bios_table_lookup[TABLE_BASE_INST_SIZE][raw_opcode_id] + bios_table_lookup[TABLE_I_W_SIZE][raw_opcode_id]*(i_w + 1);

#ifdef DEBUG_TRACE
		if (regs16[REG_CS] != last_cs) {
			printf("CS %04x -> %04x:%04x op=%02x xlat=%u\n",
				last_cs, regs16[REG_CS], reg_ip, raw_opcode_id, xlat_opcode_id);
			last_cs = regs16[REG_CS];
			fflush(stdout);
		}
#endif

		// If instruction needs to update SF, ZF and PF, set them as appropriate
		if (set_flags_type & FLAGS_UPDATE_SZP)
		{
			regs8[FLAG_SF] = SIGN_OF(op_result);
			regs8[FLAG_ZF] = !op_result;
			regs8[FLAG_PF] = bios_table_lookup[TABLE_PARITY_FLAG][(unsigned char)op_result];

			// If instruction is an arithmetic or logic operation, also set AF/OF/CF as appropriate.
			if (set_flags_type & FLAGS_UPDATE_AO_ARITH)
				set_AF_OF_arith();
			if (set_flags_type & FLAGS_UPDATE_OC_LOGIC)
				set_CF(0), set_OF(0);
		}

		// Poll timer/keyboard every KEYBOARD_TIMER_UPDATE_DELAY instructions
		if (!(++inst_counter % KEYBOARD_TIMER_UPDATE_DELAY)) {
			int8_asap = 1;
#ifdef DEBUG_TRACE
			printf("CPU n=%lu CS=%04x IP=%04x AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x SP=%04x DS=%04x ES=%04x SS=%04x IF=%u\n",
				inst_counter, regs16[REG_CS], reg_ip,
				regs16[REG_AX], regs16[REG_BX], regs16[REG_CX], regs16[REG_DX],
				regs16[REG_SI], regs16[REG_DI], regs16[REG_SP],
				regs16[REG_DS], regs16[REG_ES], regs16[REG_SS],
				regs8[FLAG_IF]);
			fflush(stdout);
#endif
		}

#ifndef NO_GRAPHICS
		// Update the video graphics display every GRAPHICS_UPDATE_DELAY instructions
		if (!(inst_counter % GRAPHICS_UPDATE_DELAY))
		{
			// Video card in graphics mode?
			if (io_ports[0x3B8] & 2)
			{
				// If we don't already have an SDL window open, set it up and compute color and video memory translation tables
				if (!sdl_screen)
				{
					for (int i = 0; i < 16; i++)
						pixel_colors[i] = mem[0x4AC] ? // CGA?
							cga_colors[(i & 12) >> 2] + (cga_colors[i & 3] << 16) // CGA -> RGB332
							: 0xFF*(((i & 1) << 24) + ((i & 2) << 15) + ((i & 4) << 6) + ((i & 8) >> 3)); // Hercules -> RGB332

					for (int i = 0; i < GRAPHICS_X * GRAPHICS_Y / 4; i++)
						vid_addr_lookup[i] = i / GRAPHICS_X * (GRAPHICS_X / 8) + (i / 2) % (GRAPHICS_X / 8) + 0x2000*(mem[0x4AC] ? (2 * i / GRAPHICS_X) % 2 : (4 * i / GRAPHICS_X) % 4);

					SDL_Init(SDL_INIT_VIDEO);
					sdl_screen = SDL_SetVideoMode(GRAPHICS_X, GRAPHICS_Y, 8, 0);
					SDL_EnableUNICODE(1);
					SDL_EnableKeyRepeat(500, 30);
				}

				// Refresh SDL display from emulated graphics card video RAM
				vid_mem_base = mem + 0xB0000 + 0x8000*(mem[0x4AC] ? 1 : io_ports[0x3B8] >> 7); // B800:0 for CGA/Hercules bank 2, B000:0 for Hercules bank 1
				for (int i = 0; i < GRAPHICS_X * GRAPHICS_Y / 4; i++)
					((unsigned *)sdl_screen->pixels)[i] = pixel_colors[15 & (vid_mem_base[vid_addr_lookup[i]] >> 4*!(i & 1))];

				SDL_Flip(sdl_screen);
			}
			else if (sdl_screen) // Application has gone back to text mode, so close the SDL window
			{
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
				sdl_screen = 0;
			}
			SDL_PumpEvents();
		}
#endif

		// Application has set trap flag, so fire INT 1
		if (trap_flag)
			pc_interrupt(1);

		trap_flag = regs8[FLAG_TF];

		// If a timer tick is pending, interrupts are enabled, and no overrides/REP are active,
		// then process the tick and check for new keystrokes
		if (int8_asap && !seg_override_en && !rep_override_en && regs8[FLAG_IF] && !regs8[FLAG_TF])
			pc_interrupt(0xA), int8_asap = 0, SDL_KEYBOARD_DRIVER;
	}

#ifndef NO_GRAPHICS
	SDL_Quit();
#endif
	close(mfd);
#ifndef _WIN32
	if (stdin_fl >= 0)
		fcntl(0, F_SETFL, stdin_fl);
#endif
	return 0;
}
