/*
 * Copyright (c) 2012 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include "offsets.h"

#if defined(__x86_64__)

#include <mach/i386/syscall_sw.h>

#ifndef VARIANT_DYLD

	.align 2, 0x90
	.globl _start_wqthread
_start_wqthread:
	// This routine is never called directly by user code, jumped from kernel
	push   %rbp
	mov    %rsp,%rbp
	sub    $24,%rsp		// align the stack
	call   __pthread_wqthread
	leave
	ret

	.align 2, 0x90
	.globl _thread_start
_thread_start:
	// This routine is never called directly by user code, jumped from kernel
	push   %rbp
	mov    %rsp,%rbp
	sub    $24,%rsp		// align the stack
	call   __pthread_start
	leave
	ret

	.align 2, 0x90
	.globl _thread_chkstk_darwin
_thread_chkstk_darwin:
	.globl ____chkstk_darwin
____chkstk_darwin: // %rax == alloca size
	pushq  %rcx
	leaq   0x10(%rsp), %rcx

	// validate that the frame pointer is on our stack (no alt stack)
	cmpq   %rcx, %gs:_PTHREAD_STRUCT_DIRECT_STACKADDR_OFFSET
	jb     Lprobe
	cmpq   %rcx, %gs:_PTHREAD_STRUCT_DIRECT_STACKBOTTOM_OFFSET
	jae    Lprobe

	// validate alloca size
	subq   %rax, %rcx
	jb     Lcrash
	cmpq   %rcx, %gs:_PTHREAD_STRUCT_DIRECT_STACKBOTTOM_OFFSET
	ja     Lcrash

	popq   %rcx
	retq

Lprobe:
	// probe the stack when it's not ours (altstack or some shenanigan)
	cmpq   $0x1000, %rax
	jb     Lend
	pushq  %rax
Lloop:
	subq   $0x1000, %rcx
	testq  %rcx, (%rcx)
	subq   $0x1000, %rax
	cmpq   $0x1000, %rax
	ja     Lloop
	popq   %rax
Lend:
	subq   %rax, %rcx
	testq  %rcx, (%rcx)

	popq   %rcx
	retq

Lcrash:
	ud2

#endif

#elif defined(__i386__)

#include <mach/i386/syscall_sw.h>

#ifndef VARIANT_DYLD

	.align 2, 0x90
	.globl _start_wqthread
_start_wqthread:
	// This routine is never called directly by user code, jumped from kernel
	push   %ebp
	mov    %esp,%ebp
	sub    $28,%esp		// align the stack
	mov    %esi,20(%esp)    //arg5
	mov    %edi,16(%esp)    //arg5
	mov    %edx,12(%esp)    //arg4
	mov    %ecx,8(%esp)             //arg3
	mov    %ebx,4(%esp)             //arg2
	mov    %eax,(%esp)              //arg1
	call   __pthread_wqthread
	leave
	ret

	.align 2, 0x90
	.globl _thread_start
_thread_start:
	// This routine is never called directly by user code, jumped from kernel
	push   %ebp
	mov    %esp,%ebp
	sub    $28,%esp		// align the stack
	mov    %esi,20(%esp)    //arg6
	mov    %edi,16(%esp)    //arg5
	mov    %edx,12(%esp)    //arg4
	mov    %ecx,8(%esp)     //arg3
	mov    %ebx,4(%esp)     //arg2
	mov    %eax,(%esp)      //arg1
	call   __pthread_start
	leave
	ret

	.align 2, 0x90
	.globl _thread_chkstk_darwin
_thread_chkstk_darwin:
	.globl ____chkstk_darwin
____chkstk_darwin: // %eax == alloca size
	pushl  %ecx
	pushl  %edx
	leal   0xc(%esp), %ecx

	// validate that the frame pointer is on our stack (no alt stack)
	movl   %gs:0x0, %edx    // pthread_self()
	cmpl   %ecx, _PTHREAD_STRUCT_DIRECT_STACKADDR_OFFSET(%edx)
	jb     Lprobe
	movl   _PTHREAD_STRUCT_DIRECT_STACKBOTTOM_OFFSET(%edx), %edx
	cmpl   %ecx, %edx
	jae    Lprobe

	// validate alloca size
	subl   %eax, %ecx
	jb     Lcrash
	cmpl   %ecx, %edx
	ja     Lcrash

	popl   %edx
	popl   %ecx
	retl

Lprobe:
	// probe the stack when it's not ours (altstack or some shenanigan)
	cmpl   $0x1000, %eax
	jb     Lend
	pushl  %eax
Lloop:
	subl   $0x1000, %ecx
	testl  %ecx, (%ecx)
	subl   $0x1000, %eax
	cmpl   $0x1000, %eax
	ja     Lloop
	popl   %eax
Lend:
	subl   %eax, %ecx
	testl  %ecx, (%ecx)

	popl   %edx
	popl   %ecx
	retl

Lcrash:
	ud2

#endif

#elif defined(__arm__)

#include <mach/arm/syscall_sw.h>

#ifndef VARIANT_DYLD

// This routine is never called directly by user code, jumped from kernel
// args 0 to 3 are already in the regs 0 to 3
// should set stack with the 2 extra args before calling pthread_wqthread()
// arg4 is in r[4]
// arg5 is in r[5]

	.text
	.align 2
	.globl _start_wqthread
_start_wqthread:
#if __ARM_ARCH_7K__
	/* align stack to 16 bytes before calling C */
	sub sp, sp, #8
#endif
	stmfd sp!, {r4, r5}
	bl __pthread_wqthread
// Stackshots will show the routine that happens to link immediately following
// _start_wqthread.  So we add an extra instruction (nop) to make stackshots
// more readable.
	nop

	.text
	.align 2
	.globl _thread_start
_thread_start:
#if __ARM_ARCH_7K__
	/* align stack to 16 bytes before calling C */
	sub sp, sp, #8
#endif
	stmfd sp!, {r4, r5}
	bl __pthread_start
// See above
	nop

#endif

#else
#error Unsupported architecture
#endif
