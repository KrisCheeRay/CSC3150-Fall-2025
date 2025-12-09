//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

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
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
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
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// TODO: complete mmap()
// ------------------------------------------------------------------
// Create a new file-backed mapping for the current process.
// Implementation style:
//   * Strict page alignment for length/offset.
//   * Bottom-up placement: find the first "hole" above p->sz that
//     does not overlap any existing VMA (differs from top-down style).
//   * Lazy allocation: do not touch the page table now; pages are
//     materialized in usertrap() on the first access (page fault).
// ------------------------------------------------------------------
uint64
sys_mmap(void)
{
  uint64 u_req_addr;        // user hint (ignored in this assignment)
  int    req_len_i;         // receive length from argint()
  int prot, flags, fd, off;
  struct file *f = 0;
  struct proc *p = myproc();

  argaddr(0, &u_req_addr);
  argint(1, &req_len_i);
  argint(2, &prot);
  argint(3, &flags);
  if (argfd(4, &fd, &f) < 0) return -1;  
  argint(5, &off);

  // Basic validation.
  if (req_len_i <= 0 || off < 0) return -1;

  // Permission checks:
  // - read requires a readable file.
  // - MAP_SHARED + PROT_WRITE requires a writable file.
  if ((prot & PROT_READ) && !f->readable) return -1;
  if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && !f->writable) return -1;

  // Page-align length and file offset.
  uint64 req_len = (uint64)req_len_i;
  uint64 len     = PGROUNDUP(req_len);
  uint   file_off = PGROUNDDOWN(off);

  // Bottom-up placement: start just above user memory (plus a gap)
  // and scan for the first non-overlapping hole among existing VMAs.
  uint64 cursor = PGROUNDUP(p->sz + 2*PGSIZE);
  uint64 chosen = 0;

  for (;;) {
    int clash = 0;
    for (int i = 0; i < VMASIZE; i++) {
      if (!p->vma[i].used) continue;
      uint64 a0 = p->vma[i].start;
      uint64 a1 = p->vma[i].start + p->vma[i].length;
      if (!(cursor + len <= a0 || cursor >= a1)) {
        clash = 1;
        cursor = PGROUNDUP(a1);
        break;
      }
    }
    if (!clash) { chosen = cursor; break; }
  }

  // Install a VMA slot. We only record metadata; no page table work now.
  for (int i = 0; i < VMASIZE; i++) {
    if (!p->vma[i].used) {
      p->vma[i].used   = 1;
      p->vma[i].start  = chosen;
      p->vma[i].length = len;
      p->vma[i].prot   = prot;
      p->vma[i].flags  = flags;
      p->vma[i].file   = filedup(f); 
      p->vma[i].offset = file_off;
      return chosen;                 
    }
  }

  // No free VMA slots.
  return -1;
}



// TODO: complete munmap()
// ------------------------------------------------------------------
// Unmap a (page-aligned) subrange of a mapping.
// Behavior:
//   * If MAP_SHARED && PROT_WRITE, conservatively write back each
//     mapped page (wrapped in fs transactions) before unmapping.
//   * Always unmap with do_free=1 since we do not implement physical
//     page sharing in this assignment.
//   * Support front-/back-trim updates to a single VMA. Splitting a
//     VMA in the middle is not implemented (return -1 for that case).
// ------------------------------------------------------------------
uint64
sys_munmap(void)
{
  uint64 u_addr;
  int    req_len_i;
  struct proc *p = myproc();

  argaddr(0, &u_addr);
  argint(1, &req_len_i);
  if (req_len_i <= 0) return -1;

  // Normalize the unmap range to page boundaries.
  uint64 unmap_start = PGROUNDDOWN(u_addr);
  uint64 unmap_end   = PGROUNDUP(u_addr + (uint64)req_len_i);

  // Find the VMA that contains unmap_start.
  int hit = -1;
  for (int i = 0; i < VMASIZE; i++) {
    if (!p->vma[i].used) continue;
    uint64 a0 = p->vma[i].start;
    uint64 a1 = p->vma[i].start + p->vma[i].length;
    if (unmap_start >= a0 && unmap_start < a1) { hit = i; break; }
  }
  if (hit < 0) return -1;

  struct vma *v = &p->vma[hit];
  uint64 v_end = v->start + v->length;
  if (unmap_end > v_end) return -1;

  // MAP_SHARED + PROT_WRITE: conservative write-back per mapped page.
  // Each page write is wrapped by begin_op()/end_op() to avoid
  // "log_write outside of trans" panics.
  if ((v->flags & MAP_SHARED) && (v->prot & PROT_WRITE)) {
    for (uint64 va = unmap_start; va < unmap_end; va += PGSIZE) {
      pte_t *pte = walk(p->pagetable, va, 0);
      if (!pte || !(*pte & PTE_V)) continue;

      uint off = v->offset + (uint)(va - v->start);
      begin_op();
      ilock(v->file->ip);
      (void)writei(v->file->ip, 1, va, off, PGSIZE);
      iunlock(v->file->ip);
      end_op();
    }
  }

  // Tear down mappings and free physical pages.
  uint64 npages = (unmap_end - unmap_start) / PGSIZE;
  uvmunmap(p->pagetable, unmap_start, npages, 1);

  // Adjust or release the VMA metadata.
  if (unmap_start == v->start && unmap_end == v_end) {
    // Full unmap of the VMA.
    fileclose(v->file);
    v->used = 0;
  } else if (unmap_start == v->start) {
    // Front-trim: advance start/offset, shrink length.
    uint64 cut = unmap_end - v->start;
    v->start  += cut;
    v->offset += (uint)cut;
    v->length -= cut;
  } else if (unmap_end == v_end) {
    // Back-trim: simply shrink the length.
    v->length = unmap_start - v->start;
  } else {
    // Middle-split not supported in this simplified design.
    return -1;
  }

  return 0;
}


static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
