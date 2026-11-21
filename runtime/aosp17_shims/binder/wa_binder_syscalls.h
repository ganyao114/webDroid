#pragma once

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int wa_binder_open(const char* path, int flags);
int wa_binder_access(const char* path, int mode);
int wa_binder_close(int fd);
void* wa_binder_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int wa_binder_munmap(void* address, size_t length);
int wa_binder_ioctl(int fd, unsigned long request, void* argument);
