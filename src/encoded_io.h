#ifndef XFSTESTS_ENCODED_IO_H
#define XFSTESTS_ENCODED_IO_H

#include <fcntl.h>

#ifndef O_ALLOW_ENCODED
#if defined(__alpha__)
#define O_ALLOW_ENCODED      0200000000
#elif defined(__hppa__)
#define O_ALLOW_ENCODED      0100000000
#elif defined(__sparc__)
#define O_ALLOW_ENCODED      0x8000000
#else
#define O_ALLOW_ENCODED      040000000
#endif
#endif

#endif /* XFSTESTS_ENCODED_IO_H */
