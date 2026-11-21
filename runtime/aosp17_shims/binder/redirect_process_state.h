#pragma once

#include "wa_binder_syscalls.h"

#include <android-base/unique_fd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define access wa_binder_access
#define open wa_binder_open
#define close wa_binder_close
#define mmap wa_binder_mmap
#define munmap wa_binder_munmap
#define ioctl wa_binder_ioctl
