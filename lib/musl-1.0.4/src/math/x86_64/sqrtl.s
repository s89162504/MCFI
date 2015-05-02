        .global sqrtl
        .align 16, 0x90
        .type sqrtl,@function
sqrtl:	fldt 8(%rsp)
	fsqrt
	#ret
        popq %rcx
        movl %ecx, %ecx
try:    movq %gs:0x1000, %rdi
__mcfi_bary_sqrtl:
        cmpq %rdi, %gs:(%rcx)
        jne check
        # addq $1, %fs:0x108 # icj_count
        jmpq *%rcx
check:
        movq %gs:(%rcx), %rsi
        testb $0x1, %sil
        jz die
        cmpl %esi, %edi
        jne try
die:
        leaq try(%rip), %rdi
        jmp __report_cfi_violation_for_return@PLT

        .section	.MCFIFuncInfo,"",@progbits
	.ascii	"{ sqrtl\nY x86_fp80!x86_fp80@\nR sqrtl\n}"
	.byte	0
