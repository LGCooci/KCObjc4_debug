/*
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#include "base.h"
#include "platform.h"
#include "internal.h"

#include <stdbool.h>

#if MALLOC_TARGET_64BIT

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <string.h>

// Copied from libmacho/getsecbyname.c, because we do not want to add a
// dependency on cctools/libmacho (-lmacho).  Only sync with original, never
// modify independently.
// Version, git hash/tag: cctools-987
static
// *** DO NOT MODIFY - START ***
/*
 * This routine returns the section structure for the named section in the
 * named segment for the mach_header_64 pointer passed to it if it exist.
 * Otherwise it returns zero.
 */
const struct section_64 *
my_getsectbynamefromheader_64(
struct mach_header_64 *mhp,
const char *segname,
const char *sectname)
{
	struct segment_command_64 *sgp;
	struct section_64 *sp;
	uint32_t i, j;
        
	sgp = (struct segment_command_64 *)
	      ((char *)mhp + sizeof(struct mach_header_64));
	for(i = 0; i < mhp->ncmds; i++){
	    if(sgp->cmd == LC_SEGMENT_64)
		if(strncmp(sgp->segname, segname, sizeof(sgp->segname)) == 0 ||
		   mhp->filetype == MH_OBJECT){
		    sp = (struct section_64 *)((char *)sgp +
			 sizeof(struct segment_command_64));
		    for(j = 0; j < sgp->nsects; j++){
			if(strncmp(sp->sectname, sectname,
			   sizeof(sp->sectname)) == 0 &&
			   strncmp(sp->segname, segname,
			   sizeof(sp->segname)) == 0)
			    return(sp);
			sp = (struct section_64 *)((char *)sp +
			     sizeof(struct section_64));
		    }
		}
	    sgp = (struct segment_command_64 *)((char *)sgp + sgp->cmdsize);
	}
	return((struct section_64 *)0);
}
// *** DO NOT MODIFY - END ***
#endif  // MALLOC_TARGET_64BIT

MALLOC_NOEXPORT
bool
main_image_has_section(const char* segname, const char *sectname)
{
#if MALLOC_TARGET_64BIT
	const struct mach_header* mh = _dyld_get_image_header(0);
	return my_getsectbynamefromheader_64((struct mach_header_64 *)mh, segname, sectname) != NULL;
#else
	return false;
#endif
}
