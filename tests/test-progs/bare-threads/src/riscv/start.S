.section .text
.global _start
.align 2

_start:
    # Set up stack
    la sp, stack_top

#     # Prepare leaf page table (L3): map 4 pages VA = PA
#     la t0, leaf_table
#     li t1, 0
# 1:
#     slli t2, t1, 12            # t2 = PPN = t1 << 12
#     srli t3, t2, 2             # Shift left 10 (PTE) = shift right 2 from 12
#     li t4, 0b111               # R | W | X
#     or t3, t3, t4              # t3 = (PPN << 10) | flags
#     sd t3, 0(t0)               # store at leaf_table[i]
#     addi t0, t0, 8             # next entry
#     addi t1, t1, 1
#     li t5, 4
#     blt t1, t5, 1b             # repeat 4 times

#     # Set up L2 page table
#     la t0, leaf_table
#     la t1, l2_table
#     srli t2, t0, 12            # PPN
#     slli t2, t2, 10
#     li t3, 0x1                # Valid bit
#     or t2, t2, t3
#     sd t2, 0(t1)              # one entry points to leaf_table

#     # Set up L1 (root) page table
#     la t0, l2_table
#     la t1, root_table
#     srli t2, t0, 12            # PPN
#     slli t2, t2, 10
#     li t3, 0x1
#     or t2, t2, t3
#     sd t2, 0(t1)              # root -> L2

#     # Setup satp
#     la t0, root_table
#     srli t0, t0, 12            # PPN
#     li t1, (8 << 60)           # mode = Sv39
#     or t0, t0, t1
#     csrw satp, t0
#     sfence.vma

    call main

    j .

.section .bss
.align 4096
root_table:  .space 4096
l2_table:    .space 4096
leaf_table:  .space 4096
stack:       .space 4096
stack_top: