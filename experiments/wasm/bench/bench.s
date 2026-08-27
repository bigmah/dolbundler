    .section .text
    .globl _start
_start:
# ---------------- 0x80003100: integer ALU kernel ----------------
# r3 = iterations -> returns accumulator
int_kernel:
    mtctr 3
    li    4, 0
    li    5, 1
1:  add    4, 4, 5
    addi   5, 5, 3
    rlwinm 6, 4, 5, 0, 26
    xor    4, 4, 6
    and    5, 5, 6
    addi   5, 5, 7
    srawi  7, 4, 3
    or     4, 4, 7
    bdnz   1b
    mr    3, 4
    blr

    .balign 256
# ---------------- 0x80003200: memory kernel ----------------
# r3 = guest base address, r4 = iterations
mem_kernel:
    mtctr 4
    li    5, 0
1:  lwz    6, 0(3)
    lwz    7, 4(3)
    add    6, 6, 7
    stw    6, 8(3)
    lhz    8, 12(3)
    stb    8, 16(3)
    addi   5, 5, 1
    bdnz   1b
    mr    3, 5
    blr

    .balign 256
# ---------------- 0x80003300: floating point kernel ----------------
# r3 = guest base address, r4 = iterations
fp_kernel:
    mtctr 4
1:  lfs    1, 0(3)
    lfs    2, 4(3)
    fmuls  3, 1, 2
    fadds  4, 3, 1
    fmadds 5, 4, 2, 3
    frsp   6, 5
    stfs   6, 8(3)
    bdnz   1b
    blr

    .balign 256
# ---------------- 0x80003400: call-heavy kernel ----------------
# r3 = iterations
call_kernel:
    mflr  11
    mtctr 3
    li    4, 0
1:  bl     leaf
    add    4, 4, 3
    bdnz   1b
    mr    3, 4
    mtlr  11
    blr

    .balign 64
leaf:
    addi  3, 4, 1
    rlwinm 3, 3, 1, 0, 30
    blr
    .balign 256
