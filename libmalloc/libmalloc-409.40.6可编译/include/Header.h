//
//  Header.h
//  libmalloc
//
//  Created by cooci on 2022/4/22.
//

#ifndef Header_h
#define Header_h

//( 0x00007fffffe00000ULL )
//( 0x00007fffffe00000 )
#define _COMM_PAGE64_BASE_ADDRESS      ( 0x00007fffffe00000ULL )
#define _COMM_PAGE_START_ADDRESS		_COMM_PAGE64_BASE_ADDRESS

#define	_COMM_PAGE_MEMORY_SIZE			(_COMM_PAGE_START_ADDRESS+0x038)
#define _COMM_PAGE_NCPUS  				(_COMM_PAGE_START_ADDRESS+0x022)

#define	_COMM_PAGE_PHYSICAL_CPUS		(_COMM_PAGE_START_ADDRESS+0x035)
#define	_COMM_PAGE_LOGICAL_CPUS			(_COMM_PAGE_START_ADDRESS+0x036)


//#define OS_VARIANT_NOTRESOLVED 1

#endif /* Header_h */
