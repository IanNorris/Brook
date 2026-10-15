.section .rodata
.balign 4096
.global large_payload
.type large_payload,@object
large_payload:
.fill 40*1024*1024,1,0xa5
.size large_payload,.-large_payload
.section .note.GNU-stack,"",@progbits
