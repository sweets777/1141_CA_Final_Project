.globl _start
.globl _start

.text
_start:
    c.addi16sp sp, -64
    c.addi4spn s0, sp, 16
    c.addi  s0, -16
    c.li    s1, 7
    c.swsp  s1, 0(sp)
    c.lwsp  a0, 0(sp)
    c.mv    a1, a0
    c.sw    a1, 4(s0)
    c.lw    a2, 4(s0)
    c.li    s0, 12
    c.li    s1, 10
    c.add   s0, s1
    c.sub   s0, s1
    c.and   s0, s1
    c.or    s0, s1
    c.xor   s0, s1
    c.andi  s0, 3
    c.slli  s0, 1
    c.srli  s0, 1
    c.srai  s0, 1
    c.nop
    c.li    s1, 3
loop_bnez:
    c.addi  s1, -1
    c.bnez  s1, loop_bnez
    c.lui   s1, 1
    c.swsp  ra, 60(sp)
    c.jal   print_string
    c.lwsp  ra, 60(sp)
    c.ebreak
halt:
    c.j     halt
ret_only:
    c.jr    ra
print_string:
    c.swsp  ra, 56(sp)
    c.swsp  s1, 52(sp)
    la      a0, str
    li      a7, 4
    ecall
    la      s1, ret_only
    c.jalr  s1
    c.lwsp  s1, 52(sp)
    c.lwsp  ra, 56(sp)
    c.jr    ra

.data
str:
    .asciz "RV32C_OK\n"
