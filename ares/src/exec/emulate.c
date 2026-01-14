#include "ares/emulate.h"

#include "ares/callsan.h"
#include "ares/core.h"
#include "ares/dev.h"

export u32 g_regs[32];
export u32 g_csr[4096];
export u32 g_pc;

export u32 g_mem_written_len;
export u32 g_mem_written_addr;
export u32 g_reg_written;

export bool g_exited;
export int g_exit_code;

extern u32 g_runtime_error_params[2];
extern Error g_runtime_error_type;
extern Section *g_gif;
extern u32 g_gif_used;
extern u32 g_gif_body_ptr;
extern u32 g_gif_body_len;

static int g_privilege_level = PRIV_USER;

// end is inclusive, like in Verilog
static inline u32 extr(u32 val, u32 end, u32 start) {
    // I need to do this here because shifting by >= bitsize is UB
    if (start == 0 && end == 31) return val;
    u32 mask = (1ul << (end + 1 - start)) - 1;
    return (val >> start) & mask;
}

static inline i32 sext(u32 x, int bits) {
    int m = 32 - bits;
    return ((i32)(x << m)) >> m;
}

// Taken from Fabrice Bellard's TinyEMU
static inline i32 div32(i32 a, i32 b) {
    if (b == 0) {
        return -1;
    } else if (a == (i32)(1ul << 31) && b == -1) {
        return a;
    } else {
        return a / b;
    }
}
static inline u32 divu32(u32 a, u32 b) {
    if (b == 0) {
        return -1;
    } else {
        return a / b;
    }
}
static inline i32 rem32(i32 a, i32 b) {
    if (b == 0) {
        return a;
    } else if (a == (i32)(1ul << 31) && b == -1) {
        return 0;
    } else {
        return a % b;
    }
}
static inline u32 remu32(u32 a, u32 b) {
    if (b == 0) {
        return a;
    } else {
        return a % b;
    }
}

Section *emulator_get_section(u32 addr) {
    for (size_t i = 0; i < ARES_ARRAY_LEN(&g_sections); i++) {
        Section *sec = *ARES_ARRAY_GET(&g_sections, i);
        if (addr >= sec->base && addr < sec->limit) {
            return sec;
        }
    }

    return NULL;
}

u8 *emulator_get_addr(u32 addr, int size, Section **out_sec) {
    Section *addr_sec = emulator_get_section(addr);

    if (out_sec) {
        *out_sec = addr_sec;
    }

    if (!addr_sec) {
        return NULL;
    }

    // NOTE: addr+size is one over the end of the accessed region
    // so it is correct for it to be > and not >=
    if (addr + size > addr_sec->contents.len + addr_sec->base) {
        return NULL;
    }

    return addr_sec->contents.buf + (addr - addr_sec->base);
    return NULL;
}

u32 LOAD(u32 addr, int size, bool *err) {
    Section *mem_sec;
    u8 *mem = emulator_get_addr(addr, size, &mem_sec);

    if (!mem_sec || !mem_sec->read ||
        (mem_sec->super && g_privilege_level == PRIV_USER)) {
        *err = true;
        return 0;
    }

    if (mem_sec->base == MMIO_BASE) {
        u32 ret;
        *err = !mmio_read(addr - MMIO_BASE, size, &ret);
        return ret;
    } else if (!mem) {
        *err = true;
        return 0;
    }

    u32 ret = 0;
    if (size == 1) {
        ret = mem[0];
    } else if (size == 2) {
        ret = mem[0];
        ret |= ((u32)mem[1]) << 8;
    } else if (size == 4) {
        ret = mem[0];
        ret |= ((u32)mem[1]) << 8;
        ret |= ((u32)mem[2]) << 16;
        ret |= ((u32)mem[3]) << 24;
    } else assert(!"Invalid size");
    *err = false;
    return ret;
}

void STORE(u32 addr, u32 val, int size, bool *err) {
    g_mem_written_len = size;
    g_mem_written_addr = addr;

    Section *mem_sec;
    u8 *mem = emulator_get_addr(addr, size, &mem_sec);

    if (!mem_sec || !mem_sec->write ||
        (mem_sec->super && g_privilege_level == PRIV_USER)) {
        *err = true;
        return;
    }

    if (mem_sec->base == MMIO_BASE) {
        *err = !mmio_write(addr - MMIO_BASE, size, val);
        return;
    } else if (!mem) {
        *err = true;
        return;
    }

    if (size == 1) {
        mem[0] = val;
    } else if (size == 2) {
        mem[0] = val;
        mem[1] = val >> 8;
    } else if (size == 4) {
        mem[0] = val;
        mem[1] = val >> 8;
        mem[2] = val >> 16;
        mem[3] = val >> 24;
    } else assert(!"Invalid size");
    *err = false;
}

#define GIF_STRIP_SYSCALL 100

static bool gif_strip_header(u32 *body_ptr, u32 *body_len) {
    if (!g_gif || g_gif_used < 13) return false;
    u8 *buf = g_gif->contents.buf;
    if (memcmp(buf, "GIF", 3) != 0) return false;
    size_t pos = 6;
    if (g_gif_used < pos + 7) return false;
    u8 packed = buf[pos + 4];
    pos += 7;
    if (packed & 0x80) {
        size_t gct_size = 3 * (1u << ((packed & 0x07) + 1));
        if (pos + gct_size > g_gif_used) return false;
        pos += gct_size;
    }
    while (pos < g_gif_used) {
        u8 marker = buf[pos++];
        if (marker == 0x2C) {
            if (pos + 9 > g_gif_used) return false;
            u8 local_packed = buf[pos + 8];
            pos += 9;
            if (local_packed & 0x80) {
                size_t lct_size = 3 * (1u << ((local_packed & 0x07) + 1));
                if (pos + lct_size > g_gif_used) return false;
                pos += lct_size;
            }
            if (pos >= g_gif_used) return false;
            pos += 1;
            size_t data_start = pos;
            while (pos < g_gif_used) {
                u8 sub_len = buf[pos++];
                if (sub_len == 0) break;
                if (pos + sub_len > g_gif_used) return false;
                pos += sub_len;
            }
            *body_ptr = g_gif->base + (u32)data_start;
            *body_len = (u32)(pos - data_start);
            return true;
        } else if (marker == 0x21) {
            if (pos >= g_gif_used) return false;
            pos += 1;
            while (pos < g_gif_used) {
                u8 sub_len = buf[pos++];
                if (sub_len == 0) break;
                if (pos + sub_len > g_gif_used) return false;
                pos += sub_len;
            }
        } else if (marker == 0x3B) {
            break;
        } else {
            return false;
        }
    }
    return false;
}

void do_syscall(u32 inst_len) {
    u32 scause = CAUSE_U_ECALL;
    if (g_privilege_level == PRIV_SUPERVISOR) {
        scause = CAUSE_S_ECALL;
    }

    if (!ARES_ARRAY_IS_EMPTY(&g_kernel_text->contents)) {
        emulator_deliver_interrupt(CAUSE_U_ECALL);
        return;
    }

    g_reg_written = 0;

    u32 param = g_regs[10];
    if (g_regs[17] == 1) {
        // print int
        char buffer[12];
        int i = 0;
        if ((i32)param < 0) {
            putchar('-');
            param = -param;
        }
        do {
            buffer[i++] = (param % 10) + '0';
            param /= 10;
        } while (param > 0);
        while (i--) putchar(buffer[i]);
    } else if (g_regs[17] == 4) {
        // print string
        u32 i = 0;
        while (1) {
            bool err = false;
            u8 ch = LOAD(param + i, 1, &err);
            if (err) return;  // TODO: return an error?
            if (ch == 0) break;
            i++;
            putchar(ch);
        }
    } else if (g_regs[17] == 11) {
        // print char
        putchar(param);
    } else if (g_regs[17] == 34) {
        // print int hex
        putchar('0');
        putchar('x');
        for (int i = 32 - 4; i >= 0; i -= 4)
            putchar("0123456789abcdef"[(param >> i) & 15]);
    } else if (g_regs[17] == 35) {
        // print int binary
        putchar('0');
        putchar('b');
        for (int i = 31; i >= 0; i--) {
            putchar(((param >> i) & 1) ? '1' : '0');
        }
    } else if (g_regs[17] == GIF_STRIP_SYSCALL) {
        u32 body_ptr = 0;
        u32 body_len = 0;
        if (gif_strip_header(&body_ptr, &body_len)) {
            g_gif_body_ptr = body_ptr;
            g_gif_body_len = body_len;
        } else {
            g_gif_body_ptr = 0;
            g_gif_body_len = 0;
        }
        g_regs[10] = g_gif_body_ptr;
        g_regs[11] = g_gif_body_len;
        g_reg_written = 11;
    } else if (g_regs[17] == 93 || g_regs[17] == 7 || g_regs[17] == 10) {
        emu_exit();
    }

    g_pc += inst_len;
}

void do_sret() {
    // SRET is only legal in supervisor
    if (g_privilege_level != PRIV_SUPERVISOR) {
        g_runtime_error_params[0] = g_pc;
        g_runtime_error_type = ERROR_UNHANDLED_INSN;
        return;
    }
    u32 status = g_csr[CSR_MSTATUS];
    bool old_spp = status & STATUS_SPP;
    bool old_spie = status & STATUS_SPIE;
    // SIE = SPIE
    status = (status & ~STATUS_SIE) | (old_spie ? STATUS_SIE : 0);
    // SPIE = 1
    status |= STATUS_SPIE;
    // SPP = 0
    status &= ~STATUS_SPP;
    g_csr[CSR_MSTATUS] = status;
    g_privilege_level = old_spp;
    g_pc = g_csr[CSR_SEPC];
}


// TODO: trap invalid CSRs
// and make unimplemented features read-only

#define SSTATUS_MASK (STATUS_SIE|STATUS_SPIE|STATUS_SPP|STATUS_FS_MASK)
#define SUPERVISOR_INT_MASK ((1<<1)|(1<<5)|(1<<9))

u32 rdcsr(u32 csr) {
    u32 mask = -1u;
    if (csr == _CSR_SSTATUS) csr = CSR_MSTATUS, mask = SSTATUS_MASK;
    else if (csr == _CSR_SIE) csr = CSR_MIE, mask = SUPERVISOR_INT_MASK;
    else if (csr == _CSR_SIP) csr = CSR_MIP, mask = SUPERVISOR_INT_MASK;
    return g_csr[csr] & mask;
}

void wrcsr(u32 csr, u32 val) {
    // for SIP, only SSIP (software interrupts) is writable
    // since it is the way to EOI a software interrupt
    // whereas the other ones are EOI'd by the respective devices
    u32 mask = -1u;
    if (csr == _CSR_SSTATUS) csr = CSR_MSTATUS, mask = SSTATUS_MASK;
    else if (csr == _CSR_SIE) csr = CSR_MIE, mask = SUPERVISOR_INT_MASK;
    else if (csr == _CSR_SIP) csr = CSR_MIP, mask = 1u << (CAUSE_SUPERVISOR_SOFTWARE & ~CAUSE_INTERRUPT);
    g_csr[csr] = (g_csr[csr] & ~mask) | (val & mask);
}

static inline u32 encode_i(u32 opcode, u32 funct3, u32 rd, u32 rs1, i32 imm) {
    return opcode | ((rd & 0x1f) << 7) | ((funct3 & 0x7) << 12) |
           ((rs1 & 0x1f) << 15) | ((imm & 0xfff) << 20);
}

static inline u32 encode_r(u32 opcode, u32 funct7, u32 funct3, u32 rd, u32 rs1,
                           u32 rs2) {
    return opcode | ((rd & 0x1f) << 7) | ((funct3 & 0x7) << 12) |
           ((rs1 & 0x1f) << 15) | ((rs2 & 0x1f) << 20) |
           ((funct7 & 0x7f) << 25);
}

static inline u32 encode_s(u32 opcode, u32 funct3, u32 rs1, u32 rs2, i32 imm) {
    u32 uimm = (u32)imm;
    return opcode | ((uimm & 0x1f) << 7) | ((funct3 & 0x7) << 12) |
           ((rs1 & 0x1f) << 15) | ((rs2 & 0x1f) << 20) |
           (((uimm >> 5) & 0x7f) << 25);
}

static inline u32 encode_b(u32 opcode, u32 funct3, u32 rs1, u32 rs2, i32 imm) {
    u32 uimm = (u32)imm;
    return opcode | (((uimm >> 11) & 0x1) << 7) | (((uimm >> 1) & 0xf) << 8) |
           ((funct3 & 0x7) << 12) | ((rs1 & 0x1f) << 15) |
           ((rs2 & 0x1f) << 20) | (((uimm >> 5) & 0x3f) << 25) |
           (((uimm >> 12) & 0x1) << 31);
}

static inline u32 encode_u(u32 opcode, u32 rd, i32 imm) {
    return opcode | ((rd & 0x1f) << 7) | (imm & 0xfffff000);
}

static inline u32 encode_j(u32 opcode, u32 rd, i32 imm) {
    u32 uimm = (u32)imm;
    return opcode | ((rd & 0x1f) << 7) | (((uimm >> 12) & 0xff) << 12) |
           (((uimm >> 11) & 0x1) << 20) | (((uimm >> 1) & 0x3ff) << 21) |
           (((uimm >> 20) & 0x1) << 31);
}

static bool decompress_rvc(u16 inst, u32 *out) {
    if (inst == 0) return false;

    u32 opcode = inst & 0x3;
    u32 funct3 = (inst >> 13) & 0x7;

    if (opcode == 0b00) {
        if (funct3 == 0b000) {
            u32 rd = 8 + ((inst >> 2) & 0x7);
            u32 imm = 0;
            imm |= ((inst >> 6) & 0x1) << 2;
            imm |= ((inst >> 5) & 0x1) << 3;
            imm |= ((inst >> 11) & 0x1) << 4;
            imm |= ((inst >> 12) & 0x1) << 5;
            imm |= ((inst >> 7) & 0x1) << 6;
            imm |= ((inst >> 8) & 0x1) << 7;
            imm |= ((inst >> 9) & 0x1) << 8;
            imm |= ((inst >> 10) & 0x1) << 9;
            if (imm == 0) return false;
            *out = encode_i(0b0010011, 0b000, rd, 2, (i32)imm);
            return true;
        }
        if (funct3 == 0b010) {
            u32 rd = 8 + ((inst >> 2) & 0x7);
            u32 rs1 = 8 + ((inst >> 7) & 0x7);
            u32 imm = 0;
            imm |= ((inst >> 6) & 0x1) << 2;
            imm |= ((inst >> 10) & 0x7) << 3;
            imm |= ((inst >> 5) & 0x1) << 6;
            *out = encode_i(0b0000011, 0b010, rd, rs1, (i32)imm);
            return true;
        }
        if (funct3 == 0b110) {
            u32 rs2 = 8 + ((inst >> 2) & 0x7);
            u32 rs1 = 8 + ((inst >> 7) & 0x7);
            u32 imm = 0;
            imm |= ((inst >> 6) & 0x1) << 2;
            imm |= ((inst >> 10) & 0x7) << 3;
            imm |= ((inst >> 5) & 0x1) << 6;
            *out = encode_s(0b0100011, 0b010, rs1, rs2, (i32)imm);
            return true;
        }
        return false;
    }

    if (opcode == 0b01) {
        if (funct3 == 0b000) {
            u32 rd = (inst >> 7) & 0x1f;
            i32 imm = sext(((inst >> 2) & 0x1f) | ((inst >> 12) & 0x1) << 5, 6);
            *out = encode_i(0b0010011, 0b000, rd, rd, imm);
            return true;
        }
        if (funct3 == 0b001) {
            i32 imm = 0;
            imm |= ((inst >> 12) & 0x1) << 11;
            imm |= ((inst >> 11) & 0x1) << 4;
            imm |= ((inst >> 9) & 0x3) << 8;
            imm |= ((inst >> 8) & 0x1) << 10;
            imm |= ((inst >> 7) & 0x1) << 6;
            imm |= ((inst >> 6) & 0x1) << 7;
            imm |= ((inst >> 3) & 0x7) << 1;
            imm |= ((inst >> 2) & 0x1) << 5;
            imm = sext(imm, 12);
            *out = encode_j(0b1101111, 1, imm);
            return true;
        }
        if (funct3 == 0b010) {
            u32 rd = (inst >> 7) & 0x1f;
            i32 imm = sext(((inst >> 2) & 0x1f) | ((inst >> 12) & 0x1) << 5, 6);
            *out = encode_i(0b0010011, 0b000, rd, 0, imm);
            return true;
        }
        if (funct3 == 0b011) {
            u32 rd = (inst >> 7) & 0x1f;
            if (rd == 2) {
                i32 imm = 0;
                imm |= ((inst >> 12) & 0x1) << 9;
                imm |= ((inst >> 6) & 0x1) << 4;
                imm |= ((inst >> 5) & 0x1) << 6;
                imm |= ((inst >> 4) & 0x1) << 8;
                imm |= ((inst >> 3) & 0x1) << 7;
                imm |= ((inst >> 2) & 0x1) << 5;
                imm = sext(imm, 10);
                if (imm == 0) return false;
                *out = encode_i(0b0010011, 0b000, 2, 2, imm);
                return true;
            }
            i32 imm = 0;
            imm |= ((inst >> 12) & 0x1) << 17;
            imm |= ((inst >> 2) & 0x1f) << 12;
            imm = sext(imm, 18);
            *out = encode_u(0b0110111, rd, imm);
            return true;
        }
        if (funct3 == 0b100) {
            u32 subop = (inst >> 10) & 0x3;
            u32 rd = 8 + ((inst >> 7) & 0x7);
            u32 rs2 = 8 + ((inst >> 2) & 0x7);
            u32 shamt = ((inst >> 2) & 0x1f) | ((inst >> 12) & 0x1) << 5;
            if (subop == 0b00) {
                if (shamt & 0x20) return false;
                *out = encode_i(0b0010011, 0b101, rd, rd, (i32)shamt);
                return true;
            }
            if (subop == 0b01) {
                if (shamt & 0x20) return false;
                *out = encode_i(0b0010011, 0b101, rd, rd,
                                (i32)(shamt | (0b0100000 << 5)));
                return true;
            }
            if (subop == 0b10) {
                i32 imm = sext(((inst >> 2) & 0x1f) | ((inst >> 12) & 0x1) << 5,
                               6);
                *out = encode_i(0b0010011, 0b111, rd, rd, imm);
                return true;
            }
            if ((inst >> 12) & 0x1) return false;
            u32 arith = (inst >> 5) & 0x3;
            if (arith == 0b00)
                *out = encode_r(0b0110011, 0b0100000, 0b000, rd, rd, rs2);
            else if (arith == 0b01)
                *out = encode_r(0b0110011, 0b0000000, 0b100, rd, rd, rs2);
            else if (arith == 0b10)
                *out = encode_r(0b0110011, 0b0000000, 0b110, rd, rd, rs2);
            else if (arith == 0b11)
                *out = encode_r(0b0110011, 0b0000000, 0b111, rd, rd, rs2);
            else
                return false;
            return true;
        }
        if (funct3 == 0b101) {
            i32 imm = 0;
            imm |= ((inst >> 12) & 0x1) << 11;
            imm |= ((inst >> 11) & 0x1) << 4;
            imm |= ((inst >> 9) & 0x3) << 8;
            imm |= ((inst >> 8) & 0x1) << 10;
            imm |= ((inst >> 7) & 0x1) << 6;
            imm |= ((inst >> 6) & 0x1) << 7;
            imm |= ((inst >> 3) & 0x7) << 1;
            imm |= ((inst >> 2) & 0x1) << 5;
            imm = sext(imm, 12);
            *out = encode_j(0b1101111, 0, imm);
            return true;
        }
        if (funct3 == 0b110 || funct3 == 0b111) {
            u32 rs1 = 8 + ((inst >> 7) & 0x7);
            i32 imm = 0;
            imm |= ((inst >> 12) & 0x1) << 8;
            imm |= ((inst >> 10) & 0x3) << 3;
            imm |= ((inst >> 5) & 0x3) << 6;
            imm |= ((inst >> 3) & 0x3) << 1;
            imm |= ((inst >> 2) & 0x1) << 5;
            imm = sext(imm, 9);
            u32 funct = (funct3 == 0b110) ? 0b000 : 0b001;
            *out = encode_b(0b1100011, funct, rs1, 0, imm);
            return true;
        }
        return false;
    }

    if (opcode == 0b10) {
        if (funct3 == 0b000) {
            u32 rd = (inst >> 7) & 0x1f;
            u32 shamt = ((inst >> 2) & 0x1f) | ((inst >> 12) & 0x1) << 5;
            if (rd == 0 || (shamt & 0x20)) return false;
            *out = encode_i(0b0010011, 0b001, rd, rd, (i32)shamt);
            return true;
        }
        if (funct3 == 0b010) {
            u32 rd = (inst >> 7) & 0x1f;
            if (rd == 0) return false;
            u32 imm = 0;
            imm |= ((inst >> 12) & 0x1) << 5;
            imm |= ((inst >> 4) & 0x7) << 2;
            imm |= ((inst >> 2) & 0x3) << 6;
            *out = encode_i(0b0000011, 0b010, rd, 2, (i32)imm);
            return true;
        }
        if (funct3 == 0b100) {
            u32 rs1 = (inst >> 7) & 0x1f;
            u32 rs2 = (inst >> 2) & 0x1f;
            u32 bit12 = (inst >> 12) & 0x1;
            if (!bit12) {
                if (rs2 == 0) {
                    if (rs1 == 0) return false;
                    *out = encode_i(0b1100111, 0b000, 0, rs1, 0);
                    return true;
                }
                *out = encode_r(0b0110011, 0b0000000, 0b000, rs1, 0, rs2);
                return true;
            }
            if (rs1 == 0 && rs2 == 0) {
                *out = 0x00100073;
                return true;
            }
            if (rs2 == 0) {
                if (rs1 == 0) return false;
                *out = encode_i(0b1100111, 0b000, 1, rs1, 0);
                return true;
            }
            if (rs1 == 0) return false;
            *out = encode_r(0b0110011, 0b0000000, 0b000, rs1, rs1, rs2);
            return true;
        }
        if (funct3 == 0b110) {
            u32 rs2 = (inst >> 2) & 0x1f;
            u32 imm = 0;
            imm |= ((inst >> 12) & 0x1) << 5;
            imm |= ((inst >> 11) & 0x1) << 4;
            imm |= ((inst >> 10) & 0x1) << 3;
            imm |= ((inst >> 9) & 0x1) << 2;
            imm |= ((inst >> 8) & 0x1) << 7;
            imm |= ((inst >> 7) & 0x1) << 6;
            *out = encode_s(0b0100011, 0b010, 2, rs2, (i32)imm);
            return true;
        }
        return false;
    }

    return false;
}

static void execute_inst(u32 inst, u32 inst_len) {
    u32 rd = extr(inst, 11, 7);
    u32 rs1 = extr(inst, 19, 15);
    u32 rs2 = extr(inst, 24, 20);
    u32 funct7 = extr(inst, 31, 25);
    u32 funct3 = extr(inst, 14, 12);

    i32 btype = sext((extr(inst, 31, 31) << 12) | (extr(inst, 7, 7) << 11) |
                         (extr(inst, 30, 25) << 5) | (extr(inst, 11, 8) << 1),
                     13);
    i32 stype = sext((extr(inst, 31, 25) << 5) | (extr(inst, 11, 7)), 12);
    i32 jtype = sext((extr(inst, 31, 31) << 20) | (extr(inst, 19, 12) << 12) |
                         (extr(inst, 20, 20) << 11) | (extr(inst, 30, 21) << 1),
                     21);
    i32 itype = sext(extr(inst, 31, 20), 12);
    i32 utype = extr(inst, 31, 12) << 12;

    u32 S1 = g_regs[rs1];
    u32 S2 = g_regs[rs2];
    u32 *D = &g_regs[rd];

    u32 opcode = extr(inst, 6, 0);
    bool err;

    // LUI
    if (opcode == 0b0110111) {
        *D = utype;
        g_pc += inst_len;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // AUIPC
    if (opcode == 0b0010111) {
        *D = g_pc + utype;
        g_pc += inst_len;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // JAL
    if (opcode == 0b1101111) {
        *D = g_pc + inst_len;
        g_pc += jtype;
        g_reg_written = rd;
        callsan_store(rd);
        if (rd == 1) callsan_call();
        return;
    }

    // JALR
    if (opcode == 0b1100111) {
        if (!callsan_can_load(rs1)) return;
        callsan_store(rd);
        *D = g_pc + inst_len;
        // this has to be checked before updating pc so that the highlighted pc
        // is correct
        if (rd == 0 && rs1 == 1) {  // jr ra/ret
            if (!callsan_ret()) return;
        }
        g_pc = (S1 + itype) & ~1;
        if (rd == 1) callsan_call();
        g_reg_written = rd;
        return;
    }

    // BEQ/BNE/BLT/BGE/BLTU/BGEU
    if (opcode == 0b1100011) {
        if (!callsan_can_load(rs1)) return;
        if (!callsan_can_load(rs2)) return;
        bool T = false;
        if ((funct3 >> 1) == 0) T = S1 == S2;
        else if ((funct3 >> 1) == 2) T = (i32)S1 < (i32)S2;
        else if ((funct3 >> 1) == 3) T = S1 < S2;
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        if (funct3 & 1) T = !T;
        g_pc += T ? btype : (i32)inst_len;
        return;
    }

    // LB/LH/LW/LBU/LHU
    if (opcode == 0b0000011) {
        if (!callsan_can_load(rs1)) return;

        if (funct3 == 0b000) *D = sext(LOAD(S1 + itype, 1, &err), 8);
        else if (funct3 == 0b001) *D = sext(LOAD(S1 + itype, 2, &err), 16);
        else if (funct3 == 0b010) *D = LOAD(S1 + itype, 4, &err);
        else if (funct3 == 0b100) *D = LOAD(S1 + itype, 1, &err);
        else if (funct3 == 0b101) *D = LOAD(S1 + itype, 2, &err);
        else {
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        if (err) {
            g_runtime_error_params[0] = S1 + itype;
            g_runtime_error_type = ERROR_LOAD;
            return;
        }
        if (!callsan_check_load(S1 + itype, 1 << (funct3 & 0b11))) {
            g_runtime_error_params[0] = S1 + itype;
            g_runtime_error_type = ERROR_CALLSAN_LOAD_STACK;
            return;
        }

        g_pc += inst_len;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // SB/SH/SW
    if (opcode == 0b0100011) {
        if (!callsan_can_load(rs1)) return;
        if (!callsan_can_load(rs2)) return;
        if (funct3 == 0b000) STORE(S1 + stype, S2, 1, &err);
        else if (funct3 == 0b001) STORE(S1 + stype, S2, 2, &err);
        else if (funct3 == 0b010) STORE(S1 + stype, S2, 4, &err);
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        if (err) {
            g_runtime_error_params[0] = S1 + stype;
            g_runtime_error_type = ERROR_STORE;
            return;
        }
        callsan_report_store(S1 + stype, 1 << funct3, rs2);
        g_pc += inst_len;
        return;
    }

    // non-Load I-type
    if (opcode == 0b0010011) {
        if (!callsan_can_load(rs1)) return;
        u32 shamt = itype & 31;
        if (funct3 == 0b000) *D = S1 + itype;                       // ADDI
        else if (funct3 == 0b010) *D = (i32)S1 < itype;             // SLTI
        else if (funct3 == 0b011) *D = S1 < (u32)itype;             // SLTIU
        else if (funct3 == 0b100) *D = S1 ^ itype;                  // XORI
        else if (funct3 == 0b110) *D = S1 | itype;                  // ORI
        else if (funct3 == 0b111) *D = S1 & itype;                  // ANDI
        else if (funct3 == 0b001 && funct7 == 0) *D = S1 << shamt;  // SLLI
        else if (funct3 == 0b101 && funct7 == 0) *D = S1 >> shamt;  // SRLI
        else if (funct3 == 0b101 && funct7 == 32)
            *D = (i32)S1 >> shamt;  // SRAI
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        g_pc += inst_len;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }

    // R-type
    if (opcode == 0b0110011) {
        if (!callsan_can_load(rs1)) return;
        if (!callsan_can_load(rs2)) return;
        u32 shamt = S2 & 31;
        if (funct3 == 0b000 && funct7 == 0) *D = S1 + S2;                 // ADD
        else if (funct3 == 0b000 && funct7 == 32) *D = S1 - S2;           // SUB
        else if (funct3 == 0b001 && funct7 == 0) *D = S1 << shamt;        // SLL
        else if (funct3 == 0b010 && funct7 == 0) *D = (i32)S1 < (i32)S2;  // SLT
        else if (funct3 == 0b011 && funct7 == 0) *D = S1 < S2;            // SLTU
        else if (funct3 == 0b100 && funct7 == 0) *D = S1 ^ S2;            // XOR
        else if (funct3 == 0b101 && funct7 == 0) *D = S1 >> shamt;        // SRL
        else if (funct3 == 0b101 && funct7 == 32) *D = (i32)S1 >> shamt;  // SRA
        else if (funct3 == 0b110 && funct7 == 0) *D = S1 | S2;            // OR
        else if (funct3 == 0b111 && funct7 == 0) *D = S1 & S2;            // AND
        else if (funct3 == 0b000 && funct7 == 1) *D = (i32)S1 * (i32)S2;  // MUL
        else if (funct3 == 0b001 && funct7 == 1)
            *D = ((i64)(i32)S1 * (i64)(i32)S2) >> 32;  // MULH
        else if (funct3 == 0b010 && funct7 == 1)
            *D = ((i64)(i32)S1 * (i64)(u32)S2) >> 32;  // MULHSU
        else if (funct3 == 0b011 && funct7 == 1)
            *D = ((u64)S1 * (u64)S2) >> 32;                            // MULHU
        else if (funct3 == 0b100 && funct7 == 1) *D = div32(S1, S2);   // DIV
        else if (funct3 == 0b101 && funct7 == 1) *D = divu32(S1, S2);  // DIVU
        else if (funct3 == 0b110 && funct7 == 1) *D = rem32(S1, S2);   // REM
        else if (funct3 == 0b111 && funct7 == 1) *D = remu32(S1, S2);  // REMU
        else {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        g_pc += inst_len;
        g_reg_written = rd;
        callsan_store(rd);
        return;
    }
    // SYSTEM instructions
    if (opcode == 0x73) {
        if (funct3 == 0b000) {
            if (itype == 0x102) {  // SRET
                do_sret();
            } else if (itype == 0x001) {  // EBREAK
                emu_exit();
                g_pc += inst_len;
            } else {  // ECALL
                do_syscall(inst_len);
            }
            return;
        } else if (funct3 == 0b001) {  // CSRRW
            u32 old = rdcsr(itype);
            if (rs1 != 0) wrcsr(itype, g_regs[rs1]);
            g_regs[rd] = old;
        } else if (funct3 == 0b010) {  // CSRRS
            u32 old = rdcsr(itype);
            if (rs1 != 0) wrcsr(itype, old | g_regs[rs1]);
            g_regs[rd] = old;
        } else if (funct3 == 0b011) {  // CSRRC
            u32 old = rdcsr(itype);
            if (rs1 != 0) wrcsr(itype, old & ~g_regs[rs1]);
            g_regs[rd] = old;
        } else if (funct3 == 0b101) {  // CSRRWI
            g_regs[rd] = g_csr[itype];
            if (rs1 != 0) wrcsr(itype, rs1);        // used as imm
        } else if (funct3 == 0b110) {  // CSRRSI
            u32 old = rdcsr(itype);
            if (rs1 != 0) wrcsr(itype, old | rs1);
            g_regs[rd] = old;
        } else if (funct3 == 0b111) {  // CSRRCI
            u32 old = rdcsr(itype);
            if (rs1 != 0) wrcsr(itype, old & ~rs1);
            g_regs[rd] = old;
        } else {
            goto end;
        }
        callsan_store(rd);

        // TODO: CSR instructions themselves are not privileged, s/m CSRs are,
        // so this is wrong, but close enough
        if (g_privilege_level == PRIV_USER) {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_PROTECTION;
        }

        g_pc += inst_len;
        g_reg_written = rd;
        return;
    }

    // if i reached here, it's an unhandled instruction
end:
    g_runtime_error_params[0] = g_pc;
    g_runtime_error_type = ERROR_UNHANDLED_INSN;
    return;
}

void emulate() {
    g_runtime_error_type = ERROR_NONE;
    g_mem_written_len = 0;
    g_reg_written = 0;
    g_regs[0] = 0;
    bool err;

    if (g_csr[CSR_MSTATUS] & STATUS_SIE) {
        u32 pending = g_csr[CSR_MIP] & g_csr[CSR_MIE];
        if (pending != 0) {
            int intno = __builtin_ctz(pending);
            emulator_deliver_interrupt(CAUSE_INTERRUPT | intno);
        }
    }

    u16 inst16 = LOAD(g_pc, 2, &err);
    if (err) {
        g_runtime_error_params[0] = g_pc;
        g_runtime_error_type = ERROR_FETCH;
        return;
    }

    if ((inst16 & 0x3) != 0x3) {
        u32 inst32 = 0;
        if (!decompress_rvc(inst16, &inst32)) {
            g_runtime_error_params[0] = g_pc;
            g_runtime_error_type = ERROR_UNHANDLED_INSN;
            return;
        }
        execute_inst(inst32, 2);
        return;
    }

    u32 inst = LOAD(g_pc, 4, &err);
    if (err) {
        g_runtime_error_params[0] = g_pc;
        g_runtime_error_type = ERROR_FETCH;
        return;
    }

    execute_inst(inst, 4);
}

// wrapper for the webui
export u32 emu_load(u32 addr, int size) {
    bool err;
    u32 val = LOAD(addr, size, &err);
    if (err) return 0;
    return val;
}

export void emu_store(u32 addr, u32 val, int size) {
    bool err;
    STORE(addr, val, size, &err);
}

void emulator_enter_kernel() {
    g_privilege_level = PRIV_SUPERVISOR;
}

void emulator_leave_kernel() {
    g_privilege_level = PRIV_USER;
}

void emulator_interrupt_set_pending(u32 intno) {
    g_csr[CSR_MIP] |= 1u << intno;
}

void emulator_interrupt_clear_pending(u32 intno) {
    g_csr[CSR_MIP] &= ~(1u << intno);
}

void emulator_deliver_interrupt(u32 cause) {
    bool is_interrupt = cause & CAUSE_INTERRUPT;
    u32 off = cause & ~CAUSE_INTERRUPT;
    assert(off < 32);

    int prev_privilege = g_privilege_level;
    
    g_csr[CSR_SEPC] = g_pc;
    g_csr[CSR_SCAUSE] = cause;

    u32 status = g_csr[CSR_MSTATUS];
    bool was_enabled = status & STATUS_SIE;
    g_privilege_level = PRIV_SUPERVISOR;

    // STATUS.xIE = 0 
    status &= ~STATUS_SIE;
    // STATUS.xPIE = STATUS.xIE of the old privilege
    status = (status & ~STATUS_SPIE) | (was_enabled ? STATUS_SPIE : 0);
    // STATUS.xPP = prev_privilege
    // NOTE: SPP is 1 bit long
    status = (status & ~STATUS_SPP) | ((prev_privilege != PRIV_USER) ? STATUS_SPP : 0);
    g_csr[CSR_MSTATUS] = status;

    u32 tvec_base = g_csr[CSR_STVEC] & ~0x3u;
    u32 tvec_mode = g_csr[CSR_STVEC] & 0x3u;
    if (tvec_mode == 1 && is_interrupt) g_pc = tvec_base + (off << 2);
    else g_pc = tvec_base;
}

void emulator_init(void) {
    g_exited = false;
    g_exit_code = 0;

    memset(g_regs, 0, sizeof(g_regs));
    g_pc = TEXT_BASE;
    g_mem_written_len = 0;
    g_mem_written_addr = 0;
    g_reg_written = 0;
    g_error_line = 0;
    g_error = NULL;

    memset(g_runtime_error_params, 0, sizeof(g_runtime_error_params));
    g_runtime_error_type = 0;

    prepare_aux_sections();

    memset(g_csr, 0, sizeof(g_csr));
    g_csr[CSR_MSTATUS] |= STATUS_SIE;
    g_csr[CSR_MIE] |= 1u << (CAUSE_SUPERVISOR_SOFTWARE & ~CAUSE_INTERRUPT);
    g_csr[CSR_MIE] |= 1u << (CAUSE_SUPERVISOR_TIMER & ~CAUSE_INTERRUPT);
    g_csr[CSR_MIE] |= 1u << (CAUSE_SUPERVISOR_EXTERNAL & ~CAUSE_INTERRUPT);
}
