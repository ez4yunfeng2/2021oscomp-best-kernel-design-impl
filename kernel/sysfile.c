//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#ifndef __DEBUG_sysfile
#undef DEBUG
#endif

#include "include/types.h"
#include "include/riscv.h"
#include "include/param.h"
#include "include/stat.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/sleeplock.h"
#include "include/file.h"
#include "include/pipe.h"
#include "include/fcntl.h"
#include "include/fs.h"
#include "include/syscall.h"
#include "include/string.h"
#include "include/printf.h"
#include "include/vm.h"
#include "include/debug.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(fd < 0)
    return -1;
  if ((f = fd2file(fd, 0)) == NULL)
    return -1;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
// static int
// fdalloc(struct file *f)
// {
//   int fd;
//   struct proc *p = myproc();

//   for(fd = 0; fd < NOFILE; fd++){
//     if(p->ofile[fd] == 0){
//       p->ofile[fd] = f;
//       return fd;
//     }
//   }
//   return -1;
// }

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_dup3(void)
{
  struct file *f;
  int old, new, flag;

  if (argfd(0, &old, &f) < 0 || argint(1, &new) < 0 || argint(2, &flag) < 0)
    return -1;
  if (old == new || new < 0)
    return -1;

  if (fdalloc3(f, new, flag) < 0)
    return -1;
  
  filedup(f);
  return new;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  f = fd2file(fd, 1);
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

uint64
sys_openat(void)
{
  char path[MAXPATH];
  int dirfd, fd, omode, fmode;
  struct file *f = NULL;
  struct inode *dp = NULL, *ip;

  if (argfd(0, &dirfd, &f) < 0) {
    if (dirfd != AT_FDCWD) {
      return -1;
    }
    dp = myproc()->cwd;
  } else {
    dp = f->ip;
  }
  if (argstr(1, path, MAXPATH) < 0 || argint(2, &omode) < 0 || argint(3, &fmode) < 0)
    return -1;

  if(omode & O_CREATE){
    ip = create(dp, path, fmode & (~I_MODE_DIR));
    if(ip == NULL){
      return -1;
    }
  } else {
    if((ip = nameifrom(dp, path)) == NULL){
      return -1;
    }
    ilock(ip);
    if ((ip->mode & I_MODE_DIR) && omode != O_RDONLY && omode != O_DIRECTORY) {
      iunlockput(ip);
      return -1;
    }
    if ((omode & O_DIRECTORY) && !(ip->mode & I_MODE_DIR)) {
      iunlockput(ip);
      return -1;
    }
  }

  if((f = filealloc()) == NULL || (fd = fdalloc(f)) < 0){
    if (f) {
      fileclose(f);
    }
    iunlockput(ip);
    return -1;
  }

  if (!(ip->mode & I_MODE_DIR) && (omode & O_TRUNC)) {
    ip->op->truncate(ip);
  }

  if (ip->dev) {
    f->type = FD_DEVICE;
  } else {
    f->type = FD_INODE;
    f->off = (omode & O_APPEND) ? ip->size : 0;
  }

  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  iunlock(ip);

  return fd;
}

uint64
sys_mkdirat(void)
{
  char path[MAXPATH];
  int dirfd, mode;
  struct file *f = NULL;
  struct inode *dp = NULL, *ip;

  if (argfd(0, &dirfd, &f) < 0) {
    if (dirfd != AT_FDCWD) {
      return -1;
    }
    dp = myproc()->cwd;
  } else {
    dp = f->ip;
  }
  if (argstr(1, path, MAXPATH) < 0 || argint(2, &mode) < 0) {
    return -1;
  }
  __debug_info("mkdirat", "create dir %s\n", path);
  if ((ip = create(dp, path, mode | I_MODE_DIR)) == NULL) {
    __debug_warn("mkdirat", "create fail\n");
    return -1;
  }
  iunlockput(ip);
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == NULL){
    return -1;
  }
  if (!(ip->mode & I_MODE_DIR)) {
    iput(ip);
    return -1;
  }
  iput(p->cwd);
  p->cwd = ip;
  return 0;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  // struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      fd2file(fd0, 1);
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  // if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
  //    copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
  if(copyout2(fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout2(fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    fd2file(fd0, 1);
    fd2file(fd1, 1);
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// To open console device.
uint64
sys_dev(void)
{
//   int fd, omode;
//   int major, minor;
//   struct file *f;

//   if(argint(0, &omode) < 0 || argint(1, &major) < 0 || argint(2, &minor) < 0){
    return -1;
//   }

//   if(omode & O_CREATE){
//     panic("dev file on FAT");
//   }

//   if(major < 0 || major >= NDEV)
//     return -1;

//   if((f = filealloc()) == NULL || (fd = fdalloc(f)) < 0){
//     if(f)
//       fileclose(f);
//     return -1;
//   }

//   f->type = FD_DEVICE;
//   f->off = 0;
//   f->ip = 0;
//   f->major = major;
//   f->readable = !(omode & O_WRONLY);
//   f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

//   return fd;
}

// To support ls command
uint64
sys_getdents(void)
{
  struct file *f;
  uint64 p, len;

  if(argfd(0, 0, &f) < 0 || argaddr(1, &p) < 0 || argint(2, (int*)&len) < 0)
    return -1;
  return filereaddir(f, p, len);
}

// get absolute cwd string
uint64
sys_getcwd(void)
{
  uint64 addr;
  int size;
  if (argaddr(0, &addr) < 0 || argint(1, (int*)&size) < 0)
    return -1;

  if (size < 2)
    return NULL;

  char buf[MAXPATH];

  int max = MAXPATH < size ? MAXPATH : size;
  if (namepath(myproc()->cwd, buf, max) < 0)
    return NULL;

  if (copyout2(addr, buf, max) < 0)
    return NULL;

  return addr;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  struct dirent dent;
  int off = 0, ret;
  while (1) {
    ret = dp->fop->readdir(dp, &dent, off);
    if (ret < 0)
      return -1;
    else if (ret == 0)
      return 1;
    else if ((dent.name[0] == '.' && dent.name[1] == '\0') ||     // skip the "." and ".."
             (dent.name[0] == '.' && dent.name[1] == '.' && dent.name[2] == '\0'))
    {
      off += ret;
    }
    else
      return 0;
  }
}

uint64
sys_unlinkat(void)
{
  char path[MAXPATH];
  int dirfd, mode;
  struct file *f = NULL;
  struct inode *dp = NULL, *ip;

  if (argfd(0, &dirfd, &f) < 0) {
    if (dirfd != AT_FDCWD) {
      return -1;
    }
    dp = myproc()->cwd;
  } else {
    dp = f->ip;
  }

  int len;
  if((len = argstr(1, path, MAXPATH)) <= 0 || argint(2, &mode) < 0)
    return -1;

  char *s = path + len - 1;
  while (s >= path && *s == '/') {
    s--;
  }
  if (s >= path && *s == '.' && (s == path || *--s == '/')) {
    return -1;
  }
  
  if((ip = nameifrom(dp, path)) == NULL){
    return -1;
  }
  if ((ip->mode ^ mode) & I_MODE_DIR) {
    iput(ip);
    return -1;
  }
  ilock(ip);
  if ((ip->mode & I_MODE_DIR) && isdirempty(ip) != 1) {
      iunlockput(ip);
      return -1;
  }
  int ret = unlink(ip);
  iunlockput(ip);

  return ret;
}

// Must hold too many locks at a time! It's possible to raise a deadlock.
// Because this op takes some steps, we can't promise
uint64
sys_rename(void)
{
//   char old[MAXPATH], new[MAXPATH];
//   if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0) {
      return -1;
//   }

//   struct inode *src = NULL, *dst = NULL, *pdst = NULL;
//   int srclock = 0;
//   char *name;
//   if ((src = ename(old)) == NULL || (pdst = enameparent(new, old)) == NULL
//       || (name = formatname(old)) == NULL) {
//     goto fail;          // src doesn't exist || dst parent doesn't exist || illegal new name
//   }
//   for (struct inode *ep = pdst; ep != NULL; ep = ep->parent) {
//     if (ep == src) {    // In what universe can we move a directory into its child?
//       goto fail;
//     }
//   }

//   uint off;
//   elock(src);     // must hold child's lock before acquiring parent's, because we do so in other similar cases
//   srclock = 1;
//   elock(pdst);
//   dst = dirlookup(pdst, name, &off);
//   if (dst != NULL) {
//     eunlock(pdst);
//     if (src == dst) {
//       goto fail;
//     } else if (src->attribute & dst->attribute & ATTR_DIRECTORY) {
//       elock(dst);
//       if (!isdirempty(dst)) {    // it's ok to overwrite an empty dir
//         eunlock(dst);
//         goto fail;
//       }
//       elock(pdst);
//       eremove(dst);
//       eunlock(dst);
//       eput(dst);
//       dst = NULL;
//     } else {                    // src is not a dir || dst exists and is not an dir
//       goto fail;
//     }
//   }

//   struct inode *psrc = src->parent;  // src must not be root, or it won't pass the for-loop test
//   memmove(src->filename, name, FAT32_MAX_FILENAME);
//   if (emake(pdst, src, off) < 0) {
//     eunlock(pdst);
//     goto fail;
//   }
//   if (psrc != pdst) {
//     elock(psrc);
//   }
//   eremove(src);
//   ereparent(pdst, src);
//   src->off = off;
//   src->valid = 1;
//   if (psrc != pdst) {
//     eunlock(psrc);
//   }
//   eunlock(pdst);
//   eunlock(src);

//   eput(psrc);   // because src no longer points to psrc
//   eput(pdst);
//   eput(src);

//   return 0;

// fail:
//   if (srclock)
//     eunlock(src);
//   if (dst)
//     eput(dst);
//   if (pdst)
//     eput(pdst);
//   if (src)
//     eput(src);
//  return -1;
}

uint64
sys_mount(void)
{
  char buf[MAXPATH];
  struct inode *dev = NULL, *imnt = NULL;
  uint64 flag;

  if (argstr(0, buf, MAXPATH) < 0 || (dev = namei(buf)) == NULL) {
    return -1;
  }
  if (argstr(1, buf, MAXPATH) < 0 || (imnt = namei(buf)) == NULL) {
    goto fail;
  }
  // Type of fs to mount and flag.
  if (argstr(2, buf, MAXPATH) < 0 || argint(3, (int*)&flag) < 0) {
    goto fail;
  }

  // We don't plan to support this
  // char data[512];
  // if (argstr(4, data, sizeof(data)) < 0) {
  //   goto fail;
  // }

  ilock(dev);
  ilock(imnt);
  if (do_mount(dev, imnt, buf, flag, NULL) < 0) {
    iunlock(imnt);
    iunlock(dev);
    goto fail;
  }
  iunlockput(imnt);
  iunlockput(dev);

  return 0;

fail:
  if (dev) {
    iput(dev);
  }
  if (imnt) {
    iput(imnt);
  }

  return -1;
}

uint64
sys_umount(void)
{
  char buf[MAXPATH];
  struct inode *mnt = NULL;
  uint64 flag;

  if (argstr(0, buf, MAXPATH) < 0 || (mnt = namei(buf)) == NULL) {
    return -1;
  }
  if (argint(1, (int*)&flag) < 0 ||
      mnt->ref > 1) // Is there anyone else holding this inode?
  {                 // If a syscall try to umount the same mntpoint, he won't pass this.
    iput(mnt);
    return -1;
  }

  ilock(mnt);
  if (do_umount(mnt, flag) < 0) {
    iunlockput(mnt);
    return -1;
  }

  // If umount successfully, mnt is no longer available,
  // we shouldn't unlock it, or put it.

  return 0;
}

uint64
sys_mmap(void)
{
  uint64 start;
  int len, prot, flags, fd, off;
  if(argaddr(0, &start) < 0 || argint(1, &len) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argint(4, &fd) < 0 || argint(5, &off) < 0)
    return -1;
  
  if(off % PGSIZE)
    return -1;

  struct file * f = fd2file(fd, 0);
  if(!f)
    return -1;

  return _mmap(start, len, prot, flags, f, off);
}