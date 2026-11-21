#pragma once

#if defined(__APPLE__)
#include <machine/endian.h>
#else
#include_next <endian.h>
#endif
