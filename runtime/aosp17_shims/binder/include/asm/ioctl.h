#pragma once

#include <sys/ioctl.h>

// The Binder UAPI uses Linux ioctl numbers on every platform.
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#undef _IOC
#undef IOC_IN
#undef IOC_OUT
#undef IOC_INOUT
#undef IOCSIZE_MASK
#undef IOCSIZE_SHIFT
#include <asm-generic/ioctl.h>
