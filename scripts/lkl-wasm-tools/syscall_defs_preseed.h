#ifdef __NR_virtio_mmio_device_add
SYSCALL_DEFINE3(_virtio_mmio_device_add,long,base,long,size,unsigned int,irq)
#endif
#ifdef __NR_new_thread_group_leader
SYSCALL_DEFINE0(_new_thread_group_leader,)
#endif
#ifdef __NR_set_tid_address
SYSCALL_DEFINE1(_set_tid_address,int *,tidptr)
#endif
#ifdef __NR_clone3
SYSCALL_DEFINE2(_clone3,struct clone_args *,uargs,size_t,size)
#endif
#ifdef __NR_unshare
SYSCALL_DEFINE1(_unshare,unsigned long,unshare_flags)
#endif
#ifdef __NR_personality
SYSCALL_DEFINE1(_personality,unsigned int,personality)
#endif
#ifdef __NR_exit
SYSCALL_DEFINE1(_exit,int,error_code)
#endif
#ifdef __NR_exit_group
SYSCALL_DEFINE1(_exit_group,int,error_code)
#endif
#ifdef __NR_waitid
SYSCALL_DEFINE5(_waitid,int,which,pid_t,upid,struct siginfo *,infop,int,options,struct rusage *,ru)
#endif
#ifdef __NR_wait4
SYSCALL_DEFINE4(_wait4,pid_t,upid,int *,stat_addr,int,options,struct rusage *,ru)
#endif
#ifdef __NR_capget
SYSCALL_DEFINE2(_capget,cap_user_header_t,header,cap_user_data_t,dataptr)
#endif
#ifdef __NR_capset
SYSCALL_DEFINE2(_capset,cap_user_header_t,header,const cap_user_data_t,data)
#endif
#ifdef __NR_ptrace
SYSCALL_DEFINE4(_ptrace,long,request,long,pid,unsigned long,addr,unsigned long,data)
#endif
#ifdef __NR_restart_syscall
SYSCALL_DEFINE0(_restart_syscall,)
#endif
#ifdef __NR_rt_sigprocmask
SYSCALL_DEFINE4(_rt_sigprocmask,int,how,sigset_t *,nset,sigset_t *,oset,size_t,sigsetsize)
#endif
#ifdef __NR_rt_sigpending
SYSCALL_DEFINE2(_rt_sigpending,sigset_t *,uset,size_t,sigsetsize)
#endif
#ifdef __NR_rt_sigtimedwait
SYSCALL_DEFINE4(_rt_sigtimedwait,const sigset_t *,uthese,siginfo_t *,uinfo,const struct __kernel_timespec *,uts,size_t,sigsetsize)
#endif
#ifdef __NR_rt_sigtimedwait_time32
SYSCALL_DEFINE4(_rt_sigtimedwait_time32,const sigset_t *,uthese,siginfo_t *,uinfo,const struct old_timespec32 *,uts,size_t,sigsetsize)
#endif
#ifdef __NR_kill
SYSCALL_DEFINE2(_kill,pid_t,pid,int,sig)
#endif
#ifdef __NR_pidfd_send_signal
SYSCALL_DEFINE4(_pidfd_send_signal,int,pidfd,int,sig,siginfo_t *,info,unsigned int,flags)
#endif
#ifdef __NR_tgkill
SYSCALL_DEFINE3(_tgkill,pid_t,tgid,pid_t,pid,int,sig)
#endif
#ifdef __NR_tkill
SYSCALL_DEFINE2(_tkill,pid_t,pid,int,sig)
#endif
#ifdef __NR_rt_sigqueueinfo
SYSCALL_DEFINE3(_rt_sigqueueinfo,pid_t,pid,int,sig,siginfo_t *,uinfo)
#endif
#ifdef __NR_rt_tgsigqueueinfo
SYSCALL_DEFINE4(_rt_tgsigqueueinfo,pid_t,tgid,pid_t,pid,int,sig,siginfo_t *,uinfo)
#endif
#ifdef __NR_sigaltstack
SYSCALL_DEFINE2(_sigaltstack,const stack_t *,uss,stack_t *,uoss)
#endif
#ifdef __NR_rt_sigaction
SYSCALL_DEFINE4(_rt_sigaction,int,sig,const struct sigaction *,act,struct sigaction *,oact,size_t,sigsetsize)
#endif
#ifdef __NR_rt_sigsuspend
SYSCALL_DEFINE2(_rt_sigsuspend,sigset_t *,unewset,size_t,sigsetsize)
#endif
#ifdef __NR_setpriority
SYSCALL_DEFINE3(_setpriority,int,which,int,who,int,niceval)
#endif
#ifdef __NR_getpriority
SYSCALL_DEFINE2(_getpriority,int,which,int,who)
#endif
#ifdef __NR_setregid
SYSCALL_DEFINE2(_setregid,gid_t,rgid,gid_t,egid)
#endif
#ifdef __NR_setgid
SYSCALL_DEFINE1(_setgid,gid_t,gid)
#endif
#ifdef __NR_setreuid
SYSCALL_DEFINE2(_setreuid,uid_t,ruid,uid_t,euid)
#endif
#ifdef __NR_setuid
SYSCALL_DEFINE1(_setuid,uid_t,uid)
#endif
#ifdef __NR_setresuid
SYSCALL_DEFINE3(_setresuid,uid_t,ruid,uid_t,euid,uid_t,suid)
#endif
#ifdef __NR_getresuid
SYSCALL_DEFINE3(_getresuid,uid_t *,ruidp,uid_t *,euidp,uid_t *,suidp)
#endif
#ifdef __NR_setresgid
SYSCALL_DEFINE3(_setresgid,gid_t,rgid,gid_t,egid,gid_t,sgid)
#endif
#ifdef __NR_getresgid
SYSCALL_DEFINE3(_getresgid,gid_t *,rgidp,gid_t *,egidp,gid_t *,sgidp)
#endif
#ifdef __NR_setfsuid
SYSCALL_DEFINE1(_setfsuid,uid_t,uid)
#endif
#ifdef __NR_setfsgid
SYSCALL_DEFINE1(_setfsgid,gid_t,gid)
#endif
#ifdef __NR_getpid
SYSCALL_DEFINE0(_getpid,)
#endif
#ifdef __NR_gettid
SYSCALL_DEFINE0(_gettid,)
#endif
#ifdef __NR_getppid
SYSCALL_DEFINE0(_getppid,)
#endif
#ifdef __NR_getuid
SYSCALL_DEFINE0(_getuid,)
#endif
#ifdef __NR_geteuid
SYSCALL_DEFINE0(_geteuid,)
#endif
#ifdef __NR_getgid
SYSCALL_DEFINE0(_getgid,)
#endif
#ifdef __NR_getegid
SYSCALL_DEFINE0(_getegid,)
#endif
#ifdef __NR_times
SYSCALL_DEFINE1(_times,struct tms *,tbuf)
#endif
#ifdef __NR_setpgid
SYSCALL_DEFINE2(_setpgid,pid_t,pid,pid_t,pgid)
#endif
#ifdef __NR_getpgid
SYSCALL_DEFINE1(_getpgid,pid_t,pid)
#endif
#ifdef __NR_getsid
SYSCALL_DEFINE1(_getsid,pid_t,pid)
#endif
#ifdef __NR_setsid
SYSCALL_DEFINE0(_setsid,)
#endif
#ifdef __NR_newuname
SYSCALL_DEFINE1(_newuname,struct new_utsname *,name)
#endif
#ifdef __NR_sethostname
SYSCALL_DEFINE2(_sethostname,char *,name,int,len)
#endif
#ifdef __NR_setdomainname
SYSCALL_DEFINE2(_setdomainname,char *,name,int,len)
#endif
#ifdef __NR_getrlimit
SYSCALL_DEFINE2(_getrlimit,unsigned int,resource,struct rlimit *,rlim)
#endif
#ifdef __NR_prlimit64
SYSCALL_DEFINE4(_prlimit64,pid_t,pid,unsigned int,resource,const struct rlimit64 *,new_rlim,struct rlimit64 *,old_rlim)
#endif
#ifdef __NR_setrlimit
SYSCALL_DEFINE2(_setrlimit,unsigned int,resource,struct rlimit *,rlim)
#endif
#ifdef __NR_getrusage
SYSCALL_DEFINE2(_getrusage,int,who,struct rusage *,ru)
#endif
#ifdef __NR_umask
SYSCALL_DEFINE1(_umask,int,mask)
#endif
#ifdef __NR_prctl
SYSCALL_DEFINE5(_prctl,int,option,unsigned long,arg2,unsigned long,arg3,unsigned long,arg4,unsigned long,arg5)
#endif
#ifdef __NR_getcpu
SYSCALL_DEFINE3(_getcpu,unsigned *,cpup,unsigned *,nodep,struct getcpu_cache *,unused)
#endif
#ifdef __NR_sysinfo
SYSCALL_DEFINE1(_sysinfo,struct sysinfo *,info)
#endif
#ifdef __NR_pidfd_open
SYSCALL_DEFINE2(_pidfd_open,pid_t,pid,unsigned int,flags)
#endif
#ifdef __NR_pidfd_getfd
SYSCALL_DEFINE3(_pidfd_getfd,int,pidfd,int,fd,unsigned int,flags)
#endif
#ifdef __NR_setns
SYSCALL_DEFINE2(_setns,int,fd,int,flags)
#endif
#ifdef __NR_reboot
SYSCALL_DEFINE4(_reboot,int,magic1,int,magic2,unsigned int,cmd,void *,arg)
#endif
#ifdef __NR_getgroups
SYSCALL_DEFINE2(_getgroups,int,gidsetsize,gid_t *,grouplist)
#endif
#ifdef __NR_setgroups
SYSCALL_DEFINE2(_setgroups,int,gidsetsize,gid_t *,grouplist)
#endif
#ifdef __NR_sched_setscheduler
SYSCALL_DEFINE3(_sched_setscheduler,pid_t,pid,int,policy,struct sched_param *,param)
#endif
#ifdef __NR_sched_setparam
SYSCALL_DEFINE2(_sched_setparam,pid_t,pid,struct sched_param *,param)
#endif
#ifdef __NR_sched_setattr
SYSCALL_DEFINE3(_sched_setattr,pid_t,pid,struct sched_attr *,uattr,unsigned int,flags)
#endif
#ifdef __NR_sched_getscheduler
SYSCALL_DEFINE1(_sched_getscheduler,pid_t,pid)
#endif
#ifdef __NR_sched_getparam
SYSCALL_DEFINE2(_sched_getparam,pid_t,pid,struct sched_param *,param)
#endif
#ifdef __NR_sched_getattr
SYSCALL_DEFINE4(_sched_getattr,pid_t,pid,struct sched_attr *,uattr,unsigned int,usize,unsigned int,flags)
#endif
#ifdef __NR_sched_setaffinity
SYSCALL_DEFINE3(_sched_setaffinity,pid_t,pid,unsigned int,len,unsigned long *,user_mask_ptr)
#endif
#ifdef __NR_sched_getaffinity
SYSCALL_DEFINE3(_sched_getaffinity,pid_t,pid,unsigned int,len,unsigned long *,user_mask_ptr)
#endif
#ifdef __NR_sched_yield
SYSCALL_DEFINE0(_sched_yield,)
#endif
#ifdef __NR_sched_get_priority_max
SYSCALL_DEFINE1(_sched_get_priority_max,int,policy)
#endif
#ifdef __NR_sched_get_priority_min
SYSCALL_DEFINE1(_sched_get_priority_min,int,policy)
#endif
#ifdef __NR_sched_rr_get_interval
SYSCALL_DEFINE2(_sched_rr_get_interval,pid_t,pid,struct __kernel_timespec *,interval)
#endif
#ifdef __NR_sched_rr_get_interval_time32
SYSCALL_DEFINE2(_sched_rr_get_interval_time32,pid_t,pid,struct old_timespec32 *,interval)
#endif
#ifdef __NR_membarrier
SYSCALL_DEFINE3(_membarrier,int,cmd,unsigned int,flags,int,cpu_id)
#endif
#ifdef __NR_syslog
SYSCALL_DEFINE3(_syslog,int,type,char *,buf,int,len)
#endif
#ifdef __NR_gettimeofday
SYSCALL_DEFINE2(_gettimeofday,struct __kernel_old_timeval *,tv,struct timezone *,tz)
#endif
#ifdef __NR_settimeofday
SYSCALL_DEFINE2(_settimeofday,struct __kernel_old_timeval *,tv,struct timezone *,tz)
#endif
#ifdef __NR_adjtimex_time32
SYSCALL_DEFINE1(_adjtimex_time32,struct old_timex32 *,utp)
#endif
#ifdef __NR_nanosleep_time32
SYSCALL_DEFINE2(_nanosleep_time32,struct old_timespec32 *,rqtp,struct old_timespec32 *,rmtp)
#endif
#ifdef __NR_timer_create
SYSCALL_DEFINE3(_timer_create,const clockid_t,which_clock,struct sigevent *,timer_event_spec,timer_t *,created_timer_id)
#endif
#ifdef __NR_timer_gettime
SYSCALL_DEFINE2(_timer_gettime,timer_t,timer_id,struct __kernel_itimerspec *,setting)
#endif
#ifdef __NR_timer_gettime32
SYSCALL_DEFINE2(_timer_gettime32,timer_t,timer_id,struct old_itimerspec32 *,setting)
#endif
#ifdef __NR_timer_getoverrun
SYSCALL_DEFINE1(_timer_getoverrun,timer_t,timer_id)
#endif
#ifdef __NR_timer_settime
SYSCALL_DEFINE4(_timer_settime,timer_t,timer_id,int,flags,const struct __kernel_itimerspec *,new_setting,struct __kernel_itimerspec *,old_setting)
#endif
#ifdef __NR_timer_settime32
SYSCALL_DEFINE4(_timer_settime32,timer_t,timer_id,int,flags,struct old_itimerspec32 *,new,struct old_itimerspec32 *,old)
#endif
#ifdef __NR_timer_delete
SYSCALL_DEFINE1(_timer_delete,timer_t,timer_id)
#endif
#ifdef __NR_clock_settime
SYSCALL_DEFINE2(_clock_settime,const clockid_t,which_clock,const struct __kernel_timespec *,tp)
#endif
#ifdef __NR_clock_gettime
SYSCALL_DEFINE2(_clock_gettime,const clockid_t,which_clock,struct __kernel_timespec *,tp)
#endif
#ifdef __NR_clock_adjtime
SYSCALL_DEFINE2(_clock_adjtime,const clockid_t,which_clock,struct __kernel_timex *,utx)
#endif
#ifdef __NR_clock_getres
SYSCALL_DEFINE2(_clock_getres,const clockid_t,which_clock,struct __kernel_timespec *,tp)
#endif
#ifdef __NR_clock_settime32
SYSCALL_DEFINE2(_clock_settime32,clockid_t,which_clock,struct old_timespec32 *,tp)
#endif
#ifdef __NR_clock_gettime32
SYSCALL_DEFINE2(_clock_gettime32,clockid_t,which_clock,struct old_timespec32 *,tp)
#endif
#ifdef __NR_clock_adjtime32
SYSCALL_DEFINE2(_clock_adjtime32,clockid_t,which_clock,struct old_timex32 *,utp)
#endif
#ifdef __NR_clock_getres_time32
SYSCALL_DEFINE2(_clock_getres_time32,clockid_t,which_clock,struct old_timespec32 *,tp)
#endif
#ifdef __NR_clock_nanosleep
SYSCALL_DEFINE4(_clock_nanosleep,const clockid_t,which_clock,int,flags,const struct __kernel_timespec *,rqtp,struct __kernel_timespec *,rmtp)
#endif
#ifdef __NR_clock_nanosleep_time32
SYSCALL_DEFINE4(_clock_nanosleep_time32,clockid_t,which_clock,int,flags,struct old_timespec32 *,rqtp,struct old_timespec32 *,rmtp)
#endif
#ifdef __NR_getitimer
SYSCALL_DEFINE2(_getitimer,int,which,struct __kernel_old_itimerval *,value)
#endif
#ifdef __NR_setitimer
SYSCALL_DEFINE3(_setitimer,int,which,struct __kernel_old_itimerval *,value,struct __kernel_old_itimerval *,ovalue)
#endif
#ifdef __NR_cachestat
SYSCALL_DEFINE4(_cachestat,unsigned int,fd,struct cachestat_range *,cstat_range,struct cachestat *,cstat,unsigned int,flags)
#endif
#ifdef __NR_process_mrelease
SYSCALL_DEFINE2(_process_mrelease,int,pidfd,unsigned int,flags)
#endif
#ifdef __NR_readahead
SYSCALL_DEFINE3(_readahead,int,fd,loff_t,offset,size_t,count)
#endif
#ifdef __NR_brk
SYSCALL_DEFINE1(_brk,unsigned long,brk)
#endif
#ifdef __NR_mmap_pgoff
SYSCALL_DEFINE6(_mmap_pgoff,unsigned long,addr,unsigned long,len,unsigned long,prot,unsigned long,flags,unsigned long,fd,unsigned long,pgoff)
#endif
#ifdef __NR_munmap
SYSCALL_DEFINE2(_munmap,unsigned long,addr,size_t,len)
#endif
#ifdef __NR_mremap
SYSCALL_DEFINE5(_mremap,unsigned long,addr,unsigned long,old_len,unsigned long,new_len,unsigned long,flags,unsigned long,new_addr)
#endif
#ifdef __NR_truncate
SYSCALL_DEFINE2(_truncate,const char *,path,long,length)
#endif
#ifdef __NR_ftruncate
SYSCALL_DEFINE2(_ftruncate,unsigned int,fd,off_t,length)
#endif
#ifdef __NR_truncate64
SYSCALL_DEFINE2(_truncate64,const char *,path,loff_t,length)
#endif
#ifdef __NR_ftruncate64
SYSCALL_DEFINE2(_ftruncate64,unsigned int,fd,loff_t,length)
#endif
#ifdef __NR_fallocate
SYSCALL_DEFINE4(_fallocate,int,fd,int,mode,loff_t,offset,loff_t,len)
#endif
#ifdef __NR_faccessat
SYSCALL_DEFINE3(_faccessat,int,dfd,const char *,filename,int,mode)
#endif
#ifdef __NR_faccessat2
SYSCALL_DEFINE4(_faccessat2,int,dfd,const char *,filename,int,mode,int,flags)
#endif
#ifdef __NR_access
SYSCALL_DEFINE2(_access,const char *,filename,int,mode)
#endif
#ifdef __NR_chdir
SYSCALL_DEFINE1(_chdir,const char *,filename)
#endif
#ifdef __NR_fchdir
SYSCALL_DEFINE1(_fchdir,unsigned int,fd)
#endif
#ifdef __NR_chroot
SYSCALL_DEFINE1(_chroot,const char *,filename)
#endif
#ifdef __NR_fchmod
SYSCALL_DEFINE2(_fchmod,unsigned int,fd,umode_t,mode)
#endif
#ifdef __NR_fchmodat2
SYSCALL_DEFINE4(_fchmodat2,int,dfd,const char *,filename,umode_t,mode,unsigned int,flags)
#endif
#ifdef __NR_fchmodat
SYSCALL_DEFINE3(_fchmodat,int,dfd,const char *,filename,umode_t,mode)
#endif
#ifdef __NR_chmod
SYSCALL_DEFINE2(_chmod,const char *,filename,umode_t,mode)
#endif
#ifdef __NR_fchownat
SYSCALL_DEFINE5(_fchownat,int,dfd,const char *,filename,uid_t,user,gid_t,group,int,flag)
#endif
#ifdef __NR_chown
SYSCALL_DEFINE3(_chown,const char *,filename,uid_t,user,gid_t,group)
#endif
#ifdef __NR_lchown
SYSCALL_DEFINE3(_lchown,const char *,filename,uid_t,user,gid_t,group)
#endif
#ifdef __NR_fchown
SYSCALL_DEFINE3(_fchown,unsigned int,fd,uid_t,user,gid_t,group)
#endif
#ifdef __NR_open
SYSCALL_DEFINE3(_open,const char *,filename,int,flags,umode_t,mode)
#endif
#ifdef __NR_openat
SYSCALL_DEFINE4(_openat,int,dfd,const char *,filename,int,flags,umode_t,mode)
#endif
#ifdef __NR_openat2
SYSCALL_DEFINE4(_openat2,int,dfd,const char *,filename,struct open_how *,how,size_t,usize)
#endif
#ifdef __NR_creat
SYSCALL_DEFINE2(_creat,const char *,pathname,umode_t,mode)
#endif
#ifdef __NR_close
SYSCALL_DEFINE1(_close,unsigned int,fd)
#endif
#ifdef __NR_close_range
SYSCALL_DEFINE3(_close_range,unsigned int,fd,unsigned int,max_fd,unsigned int,flags)
#endif
#ifdef __NR_vhangup
SYSCALL_DEFINE0(_vhangup,)
#endif
#ifdef __NR_lseek
SYSCALL_DEFINE3(_lseek,unsigned int,fd,off_t,offset,unsigned int,whence)
#endif
#ifdef __NR_llseek
SYSCALL_DEFINE5(_llseek,unsigned int,fd,unsigned long,offset_high,unsigned long,offset_low,loff_t *,result,unsigned int,whence)
#endif
#ifdef __NR_read
SYSCALL_DEFINE3(_read,unsigned int,fd,char *,buf,size_t,count)
#endif
#ifdef __NR_write
SYSCALL_DEFINE3(_write,unsigned int,fd,const char *,buf,size_t,count)
#endif
#ifdef __NR_pread64
SYSCALL_DEFINE4(_pread64,unsigned int,fd,char *,buf,size_t,count,loff_t,pos)
#endif
#ifdef __NR_pwrite64
SYSCALL_DEFINE4(_pwrite64,unsigned int,fd,const char *,buf,size_t,count,loff_t,pos)
#endif
#ifdef __NR_readv
SYSCALL_DEFINE3(_readv,unsigned long,fd,const struct iovec *,vec,unsigned long,vlen)
#endif
#ifdef __NR_writev
SYSCALL_DEFINE3(_writev,unsigned long,fd,const struct iovec *,vec,unsigned long,vlen)
#endif
#ifdef __NR_preadv
SYSCALL_DEFINE5(_preadv,unsigned long,fd,const struct iovec *,vec,unsigned long,vlen,unsigned long,pos_l,unsigned long,pos_h)
#endif
#ifdef __NR_preadv2
SYSCALL_DEFINE6(_preadv2,unsigned long,fd,const struct iovec *,vec,unsigned long,vlen,unsigned long,pos_l,unsigned long,pos_h,rwf_t,flags)
#endif
#ifdef __NR_pwritev
SYSCALL_DEFINE5(_pwritev,unsigned long,fd,const struct iovec *,vec,unsigned long,vlen,unsigned long,pos_l,unsigned long,pos_h)
#endif
#ifdef __NR_pwritev2
SYSCALL_DEFINE6(_pwritev2,unsigned long,fd,const struct iovec *,vec,unsigned long,vlen,unsigned long,pos_l,unsigned long,pos_h,rwf_t,flags)
#endif
#ifdef __NR_sendfile
SYSCALL_DEFINE4(_sendfile,int,out_fd,int,in_fd,off_t *,offset,size_t,count)
#endif
#ifdef __NR_sendfile64
SYSCALL_DEFINE4(_sendfile64,int,out_fd,int,in_fd,loff_t *,offset,size_t,count)
#endif
#ifdef __NR_copy_file_range
SYSCALL_DEFINE6(_copy_file_range,int,fd_in,loff_t *,off_in,int,fd_out,loff_t *,off_out,size_t,len,unsigned int,flags)
#endif
#ifdef __NR_newstat
SYSCALL_DEFINE2(_newstat,const char *,filename,struct stat *,statbuf)
#endif
#ifdef __NR_newlstat
SYSCALL_DEFINE2(_newlstat,const char *,filename,struct stat *,statbuf)
#endif
#ifdef __NR_newfstat
SYSCALL_DEFINE2(_newfstat,unsigned int,fd,struct stat *,statbuf)
#endif
#ifdef __NR_readlinkat
SYSCALL_DEFINE4(_readlinkat,int,dfd,const char *,pathname,char *,buf,int,bufsiz)
#endif
#ifdef __NR_readlink
SYSCALL_DEFINE3(_readlink,const char *,path,char *,buf,int,bufsiz)
#endif
#ifdef __NR_stat64
SYSCALL_DEFINE2(_stat64,const char *,filename,struct stat64 *,statbuf)
#endif
#ifdef __NR_lstat64
SYSCALL_DEFINE2(_lstat64,const char *,filename,struct stat64 *,statbuf)
#endif
#ifdef __NR_fstat64
SYSCALL_DEFINE2(_fstat64,unsigned long,fd,struct stat64 *,statbuf)
#endif
#ifdef __NR_fstatat64
SYSCALL_DEFINE4(_fstatat64,int,dfd,const char *,filename,struct stat64 *,statbuf,int,flag)
#endif
#ifdef __NR_statx
SYSCALL_DEFINE5(_statx,int,dfd,const char *,filename,unsigned,flags,unsigned int,mask,struct statx *,buffer)
#endif
#ifdef __NR_execve
SYSCALL_DEFINE3(_execve,const char *,filename,const char *const *,argv,const char *const *,envp)
#endif
#ifdef __NR_execveat
SYSCALL_DEFINE5(_execveat,int,fd,const char *,filename,const char *const *,argv,const char *const *,envp,int,flags)
#endif
#ifdef __NR_pipe2
SYSCALL_DEFINE2(_pipe2,int *,fildes,int,flags)
#endif
#ifdef __NR_pipe
SYSCALL_DEFINE1(_pipe,int *,fildes)
#endif
#ifdef __NR_mknodat
SYSCALL_DEFINE4(_mknodat,int,dfd,const char *,filename,umode_t,mode,unsigned int,dev)
#endif
#ifdef __NR_mknod
SYSCALL_DEFINE3(_mknod,const char *,filename,umode_t,mode,unsigned,dev)
#endif
#ifdef __NR_mkdirat
SYSCALL_DEFINE3(_mkdirat,int,dfd,const char *,pathname,umode_t,mode)
#endif
#ifdef __NR_mkdir
SYSCALL_DEFINE2(_mkdir,const char *,pathname,umode_t,mode)
#endif
#ifdef __NR_rmdir
SYSCALL_DEFINE1(_rmdir,const char *,pathname)
#endif
#ifdef __NR_unlinkat
SYSCALL_DEFINE3(_unlinkat,int,dfd,const char *,pathname,int,flag)
#endif
#ifdef __NR_unlink
SYSCALL_DEFINE1(_unlink,const char *,pathname)
#endif
#ifdef __NR_symlinkat
SYSCALL_DEFINE3(_symlinkat,const char *,oldname,int,newdfd,const char *,newname)
#endif
#ifdef __NR_symlink
SYSCALL_DEFINE2(_symlink,const char *,oldname,const char *,newname)
#endif
#ifdef __NR_linkat
SYSCALL_DEFINE5(_linkat,int,olddfd,const char *,oldname,int,newdfd,const char *,newname,int,flags)
#endif
#ifdef __NR_link
SYSCALL_DEFINE2(_link,const char *,oldname,const char *,newname)
#endif
#ifdef __NR_renameat2
SYSCALL_DEFINE5(_renameat2,int,olddfd,const char *,oldname,int,newdfd,const char *,newname,unsigned int,flags)
#endif
#ifdef __NR_renameat
SYSCALL_DEFINE4(_renameat,int,olddfd,const char *,oldname,int,newdfd,const char *,newname)
#endif
#ifdef __NR_rename
SYSCALL_DEFINE2(_rename,const char *,oldname,const char *,newname)
#endif
#ifdef __NR_fcntl
SYSCALL_DEFINE3(_fcntl,unsigned int,fd,unsigned int,cmd,unsigned long,arg)
#endif
#ifdef __NR_fcntl64
SYSCALL_DEFINE3(_fcntl64,unsigned int,fd,unsigned int,cmd,unsigned long,arg)
#endif
#ifdef __NR_ioctl
SYSCALL_DEFINE3(_ioctl,unsigned int,fd,unsigned int,cmd,unsigned long,arg)
#endif
#ifdef __NR_getdents
SYSCALL_DEFINE3(_getdents,unsigned int,fd,struct linux_dirent *,dirent,unsigned int,count)
#endif
#ifdef __NR_getdents64
SYSCALL_DEFINE3(_getdents64,unsigned int,fd,struct linux_dirent64 *,dirent,unsigned int,count)
#endif
#ifdef __NR_select
SYSCALL_DEFINE5(_select,int,n,fd_set *,inp,fd_set *,outp,fd_set *,exp,struct __kernel_old_timeval *,tvp)
#endif
#ifdef __NR_pselect6
SYSCALL_DEFINE6(_pselect6,int,n,fd_set *,inp,fd_set *,outp,fd_set *,exp,struct __kernel_timespec *,tsp,void *,sig)
#endif
#ifdef __NR_pselect6_time32
SYSCALL_DEFINE6(_pselect6_time32,int,n,fd_set *,inp,fd_set *,outp,fd_set *,exp,struct old_timespec32 *,tsp,void *,sig)
#endif
#ifdef __NR_poll
SYSCALL_DEFINE3(_poll,struct pollfd *,ufds,unsigned int,nfds,int,timeout_msecs)
#endif
#ifdef __NR_ppoll
SYSCALL_DEFINE5(_ppoll,struct pollfd *,ufds,unsigned int,nfds,struct __kernel_timespec *,tsp,const sigset_t *,sigmask,size_t,sigsetsize)
#endif
#ifdef __NR_ppoll_time32
SYSCALL_DEFINE5(_ppoll_time32,struct pollfd *,ufds,unsigned int,nfds,struct old_timespec32 *,tsp,const sigset_t *,sigmask,size_t,sigsetsize)
#endif
#ifdef __NR_dup3
SYSCALL_DEFINE3(_dup3,unsigned int,oldfd,unsigned int,newfd,int,flags)
#endif
#ifdef __NR_dup2
SYSCALL_DEFINE2(_dup2,unsigned int,oldfd,unsigned int,newfd)
#endif
#ifdef __NR_dup
SYSCALL_DEFINE1(_dup,unsigned int,fildes)
#endif
#ifdef __NR_umount
SYSCALL_DEFINE2(_umount,char *,name,int,flags)
#endif
#ifdef __NR_open_tree
SYSCALL_DEFINE3(_open_tree,int,dfd,const char *,filename,unsigned,flags)
#endif
#ifdef __NR_mount
SYSCALL_DEFINE5(_mount,char *,dev_name,char *,dir_name,char *,type,unsigned long,flags,void *,data)
#endif
#ifdef __NR_fsmount
SYSCALL_DEFINE3(_fsmount,int,fs_fd,unsigned int,flags,unsigned int,attr_flags)
#endif
#ifdef __NR_move_mount
SYSCALL_DEFINE5(_move_mount,int,from_dfd,const char *,from_pathname,int,to_dfd,const char *,to_pathname,unsigned int,flags)
#endif
#ifdef __NR_pivot_root
SYSCALL_DEFINE2(_pivot_root,const char *,new_root,const char *,put_old)
#endif
#ifdef __NR_mount_setattr
SYSCALL_DEFINE5(_mount_setattr,int,dfd,const char *,path,unsigned int,flags,struct mount_attr *,uattr,size_t,usize)
#endif
#ifdef __NR_statmount
SYSCALL_DEFINE4(_statmount,const struct mnt_id_req *,req,struct statmount *,buf,size_t,bufsize,unsigned int,flags)
#endif
#ifdef __NR_listmount
SYSCALL_DEFINE4(_listmount,const struct mnt_id_req *,req,u64 *,mnt_ids,size_t,nr_mnt_ids,unsigned int,flags)
#endif
#ifdef __NR_setxattr
SYSCALL_DEFINE5(_setxattr,const char *,pathname,const char *,name,const void *,value,size_t,size,int,flags)
#endif
#ifdef __NR_lsetxattr
SYSCALL_DEFINE5(_lsetxattr,const char *,pathname,const char *,name,const void *,value,size_t,size,int,flags)
#endif
#ifdef __NR_fsetxattr
SYSCALL_DEFINE5(_fsetxattr,int,fd,const char *,name,const void *,value,size_t,size,int,flags)
#endif
#ifdef __NR_getxattr
SYSCALL_DEFINE4(_getxattr,const char *,pathname,const char *,name,void *,value,size_t,size)
#endif
#ifdef __NR_lgetxattr
SYSCALL_DEFINE4(_lgetxattr,const char *,pathname,const char *,name,void *,value,size_t,size)
#endif
#ifdef __NR_fgetxattr
SYSCALL_DEFINE4(_fgetxattr,int,fd,const char *,name,void *,value,size_t,size)
#endif
#ifdef __NR_listxattr
SYSCALL_DEFINE3(_listxattr,const char *,pathname,char *,list,size_t,size)
#endif
#ifdef __NR_llistxattr
SYSCALL_DEFINE3(_llistxattr,const char *,pathname,char *,list,size_t,size)
#endif
#ifdef __NR_flistxattr
SYSCALL_DEFINE3(_flistxattr,int,fd,char *,list,size_t,size)
#endif
#ifdef __NR_removexattr
SYSCALL_DEFINE2(_removexattr,const char *,pathname,const char *,name)
#endif
#ifdef __NR_lremovexattr
SYSCALL_DEFINE2(_lremovexattr,const char *,pathname,const char *,name)
#endif
#ifdef __NR_fremovexattr
SYSCALL_DEFINE2(_fremovexattr,int,fd,const char *,name)
#endif
#ifdef __NR_vmsplice
SYSCALL_DEFINE4(_vmsplice,int,fd,const struct iovec *,uiov,unsigned long,nr_segs,unsigned int,flags)
#endif
#ifdef __NR_splice
SYSCALL_DEFINE6(_splice,int,fd_in,loff_t *,off_in,int,fd_out,loff_t *,off_out,size_t,len,unsigned int,flags)
#endif
#ifdef __NR_tee
SYSCALL_DEFINE4(_tee,int,fdin,int,fdout,size_t,len,unsigned int,flags)
#endif
#ifdef __NR_sync
SYSCALL_DEFINE0(_sync,)
#endif
#ifdef __NR_syncfs
SYSCALL_DEFINE1(_syncfs,int,fd)
#endif
#ifdef __NR_fsync
SYSCALL_DEFINE1(_fsync,unsigned int,fd)
#endif
#ifdef __NR_fdatasync
SYSCALL_DEFINE1(_fdatasync,unsigned int,fd)
#endif
#ifdef __NR_sync_file_range
SYSCALL_DEFINE4(_sync_file_range,int,fd,loff_t,offset,loff_t,nbytes,unsigned int,flags)
#endif
#ifdef __NR_sync_file_range2
SYSCALL_DEFINE4(_sync_file_range2,int,fd,unsigned int,flags,loff_t,offset,loff_t,nbytes)
#endif
#ifdef __NR_utimensat
SYSCALL_DEFINE4(_utimensat,int,dfd,const char *,filename,struct __kernel_timespec *,utimes,int,flags)
#endif
#ifdef __NR_utimensat_time32
SYSCALL_DEFINE4(_utimensat_time32,unsigned int,dfd,const char *,filename,struct old_timespec32 *,t,int,flags)
#endif
#ifdef __NR_getcwd
SYSCALL_DEFINE2(_getcwd,char *,buf,unsigned long,size)
#endif
#ifdef __NR_statfs
SYSCALL_DEFINE2(_statfs,const char *,pathname,struct statfs *,buf)
#endif
#ifdef __NR_statfs64
SYSCALL_DEFINE3(_statfs64,const char *,pathname,size_t,sz,struct statfs64 *,buf)
#endif
#ifdef __NR_fstatfs
SYSCALL_DEFINE2(_fstatfs,unsigned int,fd,struct statfs *,buf)
#endif
#ifdef __NR_fstatfs64
SYSCALL_DEFINE3(_fstatfs64,unsigned int,fd,size_t,sz,struct statfs64 *,buf)
#endif
#ifdef __NR_ustat
SYSCALL_DEFINE2(_ustat,unsigned,dev,struct ustat *,ubuf)
#endif
#ifdef __NR_fsopen
SYSCALL_DEFINE2(_fsopen,const char *,_fs_name,unsigned int,flags)
#endif
#ifdef __NR_fspick
SYSCALL_DEFINE3(_fspick,int,dfd,const char *,path,unsigned int,flags)
#endif
#ifdef __NR_fsconfig
SYSCALL_DEFINE5(_fsconfig,int,fd,unsigned int,cmd,const char *,_key,const void *,_value,int,aux)
#endif
#ifdef __NR_fanotify_init
SYSCALL_DEFINE2(_fanotify_init,unsigned int,flags,unsigned int,event_f_flags)
#endif
#ifdef __NR_fanotify_mark
SYSCALL_DEFINE5(_fanotify_mark,int,fanotify_fd,unsigned int,flags,__u64,mask,int,dfd,const char *,pathname)
#endif
#ifdef __NR_epoll_create1
SYSCALL_DEFINE1(_epoll_create1,int,flags)
#endif
#ifdef __NR_epoll_create
SYSCALL_DEFINE1(_epoll_create,int,size)
#endif
#ifdef __NR_epoll_ctl
SYSCALL_DEFINE4(_epoll_ctl,int,epfd,int,op,int,fd,struct epoll_event *,event)
#endif
#ifdef __NR_epoll_wait
SYSCALL_DEFINE4(_epoll_wait,int,epfd,struct epoll_event *,events,int,maxevents,int,timeout)
#endif
#ifdef __NR_epoll_pwait
SYSCALL_DEFINE6(_epoll_pwait,int,epfd,struct epoll_event *,events,int,maxevents,int,timeout,const sigset_t *,sigmask,size_t,sigsetsize)
#endif
#ifdef __NR_epoll_pwait2
SYSCALL_DEFINE6(_epoll_pwait2,int,epfd,struct epoll_event *,events,int,maxevents,const struct __kernel_timespec *,timeout,const sigset_t *,sigmask,size_t,sigsetsize)
#endif
#ifdef __NR_eventfd2
SYSCALL_DEFINE2(_eventfd2,unsigned int,count,int,flags)
#endif
#ifdef __NR_eventfd
SYSCALL_DEFINE1(_eventfd,unsigned int,count)
#endif
#ifdef __NR_flock
SYSCALL_DEFINE2(_flock,unsigned int,fd,unsigned int,cmd)
#endif
#ifdef __NR_name_to_handle_at
SYSCALL_DEFINE5(_name_to_handle_at,int,dfd,const char *,name,struct file_handle *,handle,void *,mnt_id,int,flag)
#endif
#ifdef __NR_open_by_handle_at
SYSCALL_DEFINE3(_open_by_handle_at,int,mountdirfd,struct file_handle *,handle,int,flags)
#endif
#ifdef __NR_ioprio_set
SYSCALL_DEFINE3(_ioprio_set,int,which,int,who,int,ioprio)
#endif
#ifdef __NR_ioprio_get
SYSCALL_DEFINE2(_ioprio_get,int,which,int,who)
#endif
#ifdef __NR_io_uring_enter
SYSCALL_DEFINE6(_io_uring_enter,unsigned int,fd,u32,to_submit,u32,min_complete,u32,flags,const void *,argp,size_t,argsz)
#endif
#ifdef __NR_io_uring_setup
SYSCALL_DEFINE2(_io_uring_setup,u32,entries,struct io_uring_params *,params)
#endif
#ifdef __NR_io_uring_register
SYSCALL_DEFINE4(_io_uring_register,unsigned int,fd,unsigned int,opcode,void *,arg,unsigned int,nr_args)
#endif
#ifdef __NR_getrandom
SYSCALL_DEFINE3(_getrandom,char *,ubuf,size_t,len,unsigned int,flags)
#endif
#ifdef __NR_socket
SYSCALL_DEFINE3(_socket,int,family,int,type,int,protocol)
#endif
#ifdef __NR_socketpair
SYSCALL_DEFINE4(_socketpair,int,family,int,type,int,protocol,int *,usockvec)
#endif
#ifdef __NR_bind
SYSCALL_DEFINE3(_bind,int,fd,struct sockaddr *,umyaddr,int,addrlen)
#endif
#ifdef __NR_listen
SYSCALL_DEFINE2(_listen,int,fd,int,backlog)
#endif
#ifdef __NR_accept4
SYSCALL_DEFINE4(_accept4,int,fd,struct sockaddr *,upeer_sockaddr,int *,upeer_addrlen,int,flags)
#endif
#ifdef __NR_accept
SYSCALL_DEFINE3(_accept,int,fd,struct sockaddr *,upeer_sockaddr,int *,upeer_addrlen)
#endif
#ifdef __NR_connect
SYSCALL_DEFINE3(_connect,int,fd,struct sockaddr *,uservaddr,int,addrlen)
#endif
#ifdef __NR_getsockname
SYSCALL_DEFINE3(_getsockname,int,fd,struct sockaddr *,usockaddr,int *,usockaddr_len)
#endif
#ifdef __NR_getpeername
SYSCALL_DEFINE3(_getpeername,int,fd,struct sockaddr *,usockaddr,int *,usockaddr_len)
#endif
#ifdef __NR_sendto
SYSCALL_DEFINE6(_sendto,int,fd,void *,buff,size_t,len,unsigned int,flags,struct sockaddr *,addr,int,addr_len)
#endif
#ifdef __NR_send
SYSCALL_DEFINE4(_send,int,fd,void *,buff,size_t,len,unsigned int,flags)
#endif
#ifdef __NR_recvfrom
SYSCALL_DEFINE6(_recvfrom,int,fd,void *,ubuf,size_t,size,unsigned int,flags,struct sockaddr *,addr,int *,addr_len)
#endif
#ifdef __NR_recv
SYSCALL_DEFINE4(_recv,int,fd,void *,ubuf,size_t,size,unsigned int,flags)
#endif
#ifdef __NR_setsockopt
SYSCALL_DEFINE5(_setsockopt,int,fd,int,level,int,optname,char *,optval,int,optlen)
#endif
#ifdef __NR_getsockopt
SYSCALL_DEFINE5(_getsockopt,int,fd,int,level,int,optname,char *,optval,int *,optlen)
#endif
#ifdef __NR_shutdown
SYSCALL_DEFINE2(_shutdown,int,fd,int,how)
#endif
#ifdef __NR_sendmsg
SYSCALL_DEFINE3(_sendmsg,int,fd,struct user_msghdr *,msg,unsigned int,flags)
#endif
#ifdef __NR_sendmmsg
SYSCALL_DEFINE4(_sendmmsg,int,fd,struct mmsghdr *,mmsg,unsigned int,vlen,unsigned int,flags)
#endif
#ifdef __NR_recvmsg
SYSCALL_DEFINE3(_recvmsg,int,fd,struct user_msghdr *,msg,unsigned int,flags)
#endif
#ifdef __NR_recvmmsg
SYSCALL_DEFINE5(_recvmmsg,int,fd,struct mmsghdr *,mmsg,unsigned int,vlen,unsigned int,flags,struct __kernel_timespec *,timeout)
#endif
#ifdef __NR_recvmmsg_time32
SYSCALL_DEFINE5(_recvmmsg_time32,int,fd,struct mmsghdr *,mmsg,unsigned int,vlen,unsigned int,flags,struct old_timespec32 *,timeout)
#endif
