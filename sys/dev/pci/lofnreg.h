/*	$OpenBSD: lofnreg.h,v 1.3 2001/06/26 03:41:01 jason Exp $	*/

/*
 * Copyright (c) 2001 Jason L. Wright (jason@thought.net)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Jason L. Wright
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define	LOFN_BAR0	0x10

#define	LOFN_WIN_0	0x0000
#define	LOFN_WIN_1	0x2000
#define	LOFN_WIN_2	0x4000
#define	LOFN_WIN_3	0x6000

/* Control and status registers, relative to window number */
#define	LOFN_REL_CR		0x1fd4		/* Command */
#define	LOFN_REL_SR		0x1fd8		/* Status */
#define	LOFN_REL_IER		0x1fdc		/* Interrupt enable */
#define	LOFN_REL_RNC		0x1fe0		/* RNG config */
#define	LOFN_REL_CFG1		0x1fe4		/* Config1 */
#define	LOFN_REL_CFG2		0x1fe8		/* Config2 */
#define	LOFN_REL_CHIPID		0x1fec		/* Chip ID */

#define	LOFN_CR_ADDR_MASK	0x0000003f	/* Instruction addr offset */

#define	LOFN_SR_CARRY		0x00000008	/* Carry from operation */
#define	LOFN_SR_RNG_UF		0x00001000	/* RNG underflow */
#define	LOFN_SR_RNG_RDY		0x00004000	/* RNG ready */
#define	LOFN_SR_DONE		0x00008000	/* Operation done */

#define	LOFN_IER_RDY		0x00004000	/* RNG ready */
#define	LOFN_IER_DONE		0x00008000	/* Operation done */

#define	LOFN_RNC_OUTSCALE	0x00000080	/* Output prescalar */
#define	LOFN_RNC_1STSCALE	0x00000f00	/* First prescalar */

#define	LOFN_CFG1_RESET		0x00000001	/* Reset */
#define	LOFN_CFG1_MULTI		0x00000038	/* PLL multiple */
#define	LOFN_CFG1_CLOCK		0x00000040	/* Clock select */

#define	LOFN_CFG2_RNGENA	0x00000001	/* RNG enable */
#define	LOFN_CFG2_PRCENA	0x00000002	/* Processor enable */

#define	LOFN_CHIPID_MASK	0x0000ffff	/* Chip ID */

