/* Modified by Philip Guo for the CDE project
   (grep for 'pgbovine' to see my changes)

   The resulting executables are called 'cde' and 'cde-exec'

 */

/*
 * Copyright (c) 1991, 1992 Paul Kranenburg <pk@cs.few.eur.nl>
 * Copyright (c) 1993 Branko Lankester <branko@hacktic.nl>
 * Copyright (c) 1993, 1994, 1995, 1996 Rick Sladkey <jrs@world.std.com>
 * Copyright (c) 1996-1999 Wichert Akkerman <wichert@cistron.nl>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *	$Id$
 */

#include "defs.h"

#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <sys/param.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>

#include <sys/utsname.h> // pgbovine
#include <sys/mman.h> // pgbovine

#ifdef LINUX
# include <asm/unistd.h>
# if defined __NR_tgkill
#  define my_tgkill(pid, tid, sig) syscall (__NR_tgkill, (pid), (tid), (sig))
# elif defined __NR_tkill
#  define my_tgkill(pid, tid, sig) syscall (__NR_tkill, (tid), (sig))
# else
   /* kill() may choose arbitrarily the target task of the process group
      while we later wait on a that specific TID.  PID process waits become
      TID task specific waits for a process under ptrace(2).  */
#  warning "Neither tkill(2) nor tgkill(2) available, risk of strace hangs!"
#  define my_tgkill(pid, tid, sig) kill ((tid), (sig))
# endif
#endif

#if defined(IA64) && defined(LINUX)
# include <asm/ptrace_offsets.h>
#endif

#ifdef USE_PROCFS
#include <poll.h>
#endif

#ifdef SVR4
#include <sys/stropts.h>
#ifdef HAVE_MP_PROCFS
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#endif
#endif
extern char **environ;
extern int optind;
extern char *optarg;

// pgbovine
extern char CDE_exec_mode;
extern char CDE_provenance_mode; // -p option
extern char CDE_verbose_mode; // -v option
extern char CDE_exec_streaming_mode; // -s option
extern char CDE_use_linker_from_package; // ON by default, -l option to turn OFF
extern void alloc_tcb_CDE_fields(struct tcb* tcp);
extern void free_tcb_CDE_fields(struct tcb* tcp);
extern void copy_file(char* src_filename, char* dst_filename, int perms);
extern void strcpy_redirected_cderoot(char* dst, char* src);
extern void CDE_create_path_symlink_dirs(void);
extern void CDE_create_toplevel_symlink_dirs(void);
extern void CDE_init_tcb_dir_fields(struct tcb* tcp);
extern void CDE_exec_mode_early_init(void);
extern void CDE_create_convenience_scripts(char** argv, int optind);
extern char cde_starting_pwd[MAXPATHLEN];
extern char cde_pseudo_root_dir[MAXPATHLEN];
extern void CDE_init_options(void);
extern void CDE_init_allow_paths(void);
extern void CDE_load_environment_vars(void);
extern FILE* CDE_copied_files_logfile;
extern char* cde_root_cache_dir;
#include "okapi.h"


// only valid if !CDE_exec_mode
char* CDE_PACKAGE_DIR = NULL;
char* CDE_ROOT_DIR = NULL;


int debug = 0, followfork = 1; // pgbovine - turn on followfork by default
int dtime = 0, xflag = 0, qflag = 1; // pgbovine - turn on quiet mode (-q) by
                                     // default to shut up terminal line noise
cflag_t cflag = CFLAG_NONE;
static int iflag = 0, interactive = 0, pflag_seen = 0, rflag = 0, tflag = 0;
/*
 * daemonized_tracer supports -D option.
 * With this option, strace forks twice.
 * Unlike normal case, with -D *grandparent* process exec's,
 * becoming a traced process. Child exits (this prevents traced process
 * from having children it doesn't expect to have), and grandchild
 * attaches to grandparent similarly to strace -p PID.
 * This allows for more transparent interaction in cases
 * when process and its parent are communicating via signals,
 * wait() etc. Without -D, strace process gets lodged in between,
 * disrupting parent<->child link.
 */
static bool daemonized_tracer = 0;

/* Sometimes we want to print only succeeding syscalls. */
int not_failing_only = 0;

static int exit_code = 0;
static int strace_child = 0;

static char *username = NULL;
uid_t run_uid;
gid_t run_gid;

int acolumn = DEFAULT_ACOLUMN;
int max_strlen = DEFAULT_STRLEN;
static char *outfname = NULL;
FILE *outf;
static int curcol;
struct tcb **tcbtab;
unsigned int nprocs, tcbtabsize;
char *progname;
extern char **environ;

static int detach(struct tcb *tcp, int sig);
static int trace(void);
static void cleanup(void);
static void interrupt(int sig);
static sigset_t empty_set, blocked_set;

#ifdef HAVE_SIG_ATOMIC_T
static volatile sig_atomic_t interrupted;
#else /* !HAVE_SIG_ATOMIC_T */
static volatile int interrupted;
#endif /* !HAVE_SIG_ATOMIC_T */

#ifdef USE_PROCFS

static struct tcb *pfd2tcb(int pfd);
static void reaper(int sig);
static void rebuild_pollv(void);
static struct pollfd *pollv;

#ifndef HAVE_POLLABLE_PROCFS

static void proc_poll_open(void);
static void proc_poller(int pfd);

struct proc_pollfd {
	int fd;
	int revents;
	int pid;
};

static int poller_pid;
static int proc_poll_pipe[2] = { -1, -1 };

#endif /* !HAVE_POLLABLE_PROCFS */

#ifdef HAVE_MP_PROCFS
#define POLLWANT	POLLWRNORM
#else
#define POLLWANT	POLLPRI
#endif
#endif /* USE_PROCFS */

static void
usage(ofp, exitval)
FILE *ofp;
int exitval;
{
  if (CDE_exec_mode) {
    fprintf(ofp,
            "CDE: Code, Data, and Environment packaging for Linux\n"
            "Copyright 2010-2011 Philip Guo (pg@cs.stanford.edu)\n"
            "http://www.stanford.edu/~pgbovine/cde.html\n\n"
            "usage: cde-exec [command within cde-root/ to run]\n");

    fprintf(ofp, "\nOptions\n");
    fprintf(ofp, "  -l  : Use native dynamic linker on machine (less portable but more robust)\n");
    fprintf(ofp, "  -s  : Streaming mode (ooh, mysterious!)\n");
    fprintf(ofp, "  -v  : Verbose mode (for debugging)\n");
  }
  else {
    fprintf(ofp,
            "CDE: Code, Data, and Environment packaging for Linux\n"
            "Copyright 2010-2011 Philip Guo (pg@cs.stanford.edu)\n"
            "http://www.stanford.edu/~pgbovine/cde.html\n\n"
            "usage: cde [command to run and package]\n");

    fprintf(ofp, "\nOptions\n");
    fprintf(ofp, "  -p  : Provenance mode (output a provenance.log file)\n");
    fprintf(ofp, "  -c  : Print the order of files copied into the package in cde-copied-files.log\n");
    fprintf(ofp, "  -o <output dir> : Set a custom output directory instead of \"cde-package/\"\n");
    fprintf(ofp, "  -v  : Verbose mode (for debugging)\n");
  }

	exit(exitval);
}

#ifdef SVR4
#ifdef MIPS
void
foobar()
{
}
#endif /* MIPS */
#endif /* SVR4 */

/* Glue for systems without a MMU that cannot provide fork() */
#ifdef HAVE_FORK
# define strace_vforked 0
#else
# define strace_vforked 1
# define fork()         vfork()
#endif

static int
set_cloexec_flag(int fd)
{
	int     flags, newflags;

	if ((flags = fcntl(fd, F_GETFD, 0)) < 0)
	{
		fprintf(stderr, "%s: fcntl F_GETFD: %s\n",
			progname, strerror(errno));
		return -1;
	}

	newflags = flags | FD_CLOEXEC;
	if (flags == newflags)
		return 0;

	if (fcntl(fd, F_SETFD, newflags) < 0)
	{
		fprintf(stderr, "%s: fcntl F_SETFD: %s\n",
			progname, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * When strace is setuid executable, we have to swap uids
 * before and after filesystem and process management operations.
 */
static void
swap_uid(void)
{
#ifndef SVR4
	int euid = geteuid(), uid = getuid();

	if (euid != uid && setreuid(euid, uid) < 0)
	{
		fprintf(stderr, "%s: setreuid: %s\n",
			progname, strerror(errno));
		exit(1);
	}
#endif
}

#if _LFS64_LARGEFILE
# define fopen_for_output fopen64
#else
# define fopen_for_output fopen
#endif

static FILE *
strace_fopen(const char *path, const char *mode)
{
	FILE *fp;

	swap_uid();
	if ((fp = fopen_for_output(path, mode)) == NULL)
		fprintf(stderr, "%s: can't fopen '%s': %s\n",
			progname, path, strerror(errno));
	swap_uid();
	if (fp && set_cloexec_flag(fileno(fp)) < 0)
	{
		fclose(fp);
		fp = NULL;
	}
	return fp;
}

static int popen_pid = -1;

#ifndef _PATH_BSHELL
# define _PATH_BSHELL "/bin/sh"
#endif

/*
 * We cannot use standard popen(3) here because we have to distinguish
 * popen child process from other processes we trace, and standard popen(3)
 * does not export its child's pid.
 */
static FILE *
strace_popen(const char *command)
{
	int     fds[2];

	swap_uid();
	if (pipe(fds) < 0)
	{
		fprintf(stderr, "%s: pipe: %s\n",
			progname, strerror(errno));
		swap_uid();
		return NULL;
	}

	if (set_cloexec_flag(fds[1]) < 0)
	{
		close(fds[0]);
		close(fds[1]);
		swap_uid();
		return NULL;
	}

	if ((popen_pid = fork()) == -1)
	{
		fprintf(stderr, "%s: fork: %s\n",
			progname, strerror(errno));
		close(fds[0]);
		close(fds[1]);
		swap_uid();
		return NULL;
	}

	if (popen_pid)
	{
		/* parent */
		close(fds[0]);
		swap_uid();
		return fdopen(fds[1], "w");
	} else
	{
		/* child */
		close(fds[1]);
		if (fds[0] && (dup2(fds[0], 0) || close(fds[0])))
		{
			fprintf(stderr, "%s: dup2: %s\n",
				progname, strerror(errno));
			_exit(1);
		}
		execl(_PATH_BSHELL, "sh", "-c", command, NULL);
		fprintf(stderr, "%s: execl: %s: %s\n",
			progname, _PATH_BSHELL, strerror(errno));
		_exit(1);
	}
}

static int
newoutf(struct tcb *tcp)
{
	if (outfname && followfork > 1) {
		char name[520 + sizeof(int) * 3];
		FILE *fp;

		sprintf(name, "%.512s.%u", outfname, tcp->pid);
		if ((fp = strace_fopen(name, "w")) == NULL)
			return -1;
		tcp->outf = fp;
	}
	return 0;
}

static void
startup_attach(void)
{
	int tcbi;
	struct tcb *tcp;

	/*
	 * Block user interruptions as we would leave the traced
	 * process stopped (process state T) if we would terminate in
	 * between PTRACE_ATTACH and wait4 () on SIGSTOP.
	 * We rely on cleanup () from this point on.
	 */
	if (interactive)
		sigprocmask(SIG_BLOCK, &blocked_set, NULL);

	if (daemonized_tracer) {
		pid_t pid = fork();
		if (pid < 0) {
			_exit(1);
		}
		if (pid) { /* parent */
			/*
			 * Wait for child to attach to straced process
			 * (our parent). Child SIGKILLs us after it attached.
			 * Parent's wait() is unblocked by our death,
			 * it proceeds to exec the straced program.
			 */
			pause();
			_exit(0); /* paranoia */
		}
	}

	for (tcbi = 0; tcbi < tcbtabsize; tcbi++) {
		tcp = tcbtab[tcbi];
		if (!(tcp->flags & TCB_INUSE) || !(tcp->flags & TCB_ATTACHED))
			continue;
#ifdef LINUX
		if (tcp->flags & TCB_CLONE_THREAD)
			continue;
#endif
		/* Reinitialize the output since it may have changed. */
		tcp->outf = outf;
		if (newoutf(tcp) < 0)
			exit(1);

#ifdef USE_PROCFS
		if (proc_open(tcp, 1) < 0) {
			fprintf(stderr, "trouble opening proc file\n");
			droptcb(tcp);
			continue;
		}
#else /* !USE_PROCFS */
# ifdef LINUX
		if (followfork && !daemonized_tracer) {
			char procdir[sizeof("/proc/%d/task") + sizeof(int) * 3];
			DIR *dir;

			sprintf(procdir, "/proc/%d/task", tcp->pid);
			dir = opendir(procdir);
			if (dir != NULL) {
				unsigned int ntid = 0, nerr = 0;
				struct dirent *de;
				int tid;
				while ((de = readdir(dir)) != NULL) {
					if (de->d_fileno == 0)
						continue;
					tid = atoi(de->d_name);
					if (tid <= 0)
						continue;
					++ntid;
					if (ptrace(PTRACE_ATTACH, tid, (char *) 1, 0) < 0)
						++nerr;
					else if (tid != tcbtab[tcbi]->pid) {
						tcp = alloctcb(tid);
						tcp->flags |= TCB_ATTACHED|TCB_CLONE_THREAD|TCB_CLONE_DETACHED|TCB_FOLLOWFORK;
						tcbtab[tcbi]->nchildren++;
						tcbtab[tcbi]->nclone_threads++;
						tcbtab[tcbi]->nclone_detached++;
						tcp->parent = tcbtab[tcbi];
            CDE_init_tcb_dir_fields(tcp); // pgbovine - do it AFTER you init parent
					}
					if (interactive) {
						sigprocmask(SIG_SETMASK, &empty_set, NULL);
						if (interrupted)
							return;
						sigprocmask(SIG_BLOCK, &blocked_set, NULL);
					}
				}
				closedir(dir);
				ntid -= nerr;
				if (ntid == 0) {
					perror("attach: ptrace(PTRACE_ATTACH, ...)");
					droptcb(tcp);
					continue;
				}
				if (!qflag) {
					fprintf(stderr, ntid > 1
? "Process %u attached with %u threads - interrupt to quit\n"
: "Process %u attached - interrupt to quit\n",
						tcbtab[tcbi]->pid, ntid);
				}
				continue;
			} /* if (opendir worked) */
		} /* if (-f) */
# endif
		if (ptrace(PTRACE_ATTACH, tcp->pid, (char *) 1, 0) < 0) {
			perror("attach: ptrace(PTRACE_ATTACH, ...)");
			droptcb(tcp);
			continue;
		}
		/* INTERRUPTED is going to be checked at the top of TRACE.  */

		if (daemonized_tracer) {
			/*
			 * It is our grandparent we trace, not a -p PID.
			 * Don't want to just detach on exit, so...
			 */
			tcp->flags &= ~TCB_ATTACHED;
			/*
			 * Make parent go away.
			 * Also makes grandparent's wait() unblock.
			 */
			kill(getppid(), SIGKILL);
		}

#endif /* !USE_PROCFS */
		if (!qflag)
			fprintf(stderr,
				"Process %u attached - interrupt to quit\n",
				tcp->pid);
	}

	if (interactive)
		sigprocmask(SIG_SETMASK, &empty_set, NULL);
}

static void
startup_child (char **argv)
{
	struct stat statbuf;
	const char *filename;
	char pathname[MAXPATHLEN];
	int pid = 0;
	struct tcb *tcp;

  // pgbovine
  char path_to_search[MAXPATHLEN];
  path_to_search[0] = '\0';

	filename = argv[0];
	if (strchr(filename, '/')) {
		if (strlen(filename) > sizeof pathname - 1) {
			errno = ENAMETOOLONG;
			perror("strace: exec");
			exit(1);
		}
		strcpy(pathname, filename);
	}
#ifdef USE_DEBUGGING_EXEC
	/*
	 * Debuggers customarily check the current directory
	 * first regardless of the path but doing that gives
	 * security geeks a panic attack.
	 */
	else if (stat(filename, &statbuf) == 0)
		strcpy(pathname, filename);
#endif /* USE_DEBUGGING_EXEC */
	else {
		char *path;
		int m, n, len;

		for (path = getenv("PATH"); path && *path; path += m) {
			if (strchr(path, ':')) {
				n = strchr(path, ':') - path;
				m = n + 1;
			}
			else
				m = n = strlen(path);
			if (n == 0) {
				if (!getcwd(pathname, MAXPATHLEN))
					continue;
				len = strlen(pathname);
			}
			else if (n > sizeof pathname - 1)
				continue;
			else {
				strncpy(pathname, path, n);
				len = n;
			}
			if (len && pathname[len - 1] != '/')
				pathname[len++] = '/';
			strcpy(pathname + len, filename);

      // pgbovine
      if (CDE_exec_mode) {
        strcpy_redirected_cderoot(path_to_search, pathname);
      }
      else {
        strcpy(path_to_search, pathname);
      }
      //printf("path_to_search = '%s'\n", path_to_search);

			if (stat(path_to_search, &statbuf) == 0 &&
			    /* Accept only regular files
			       with some execute bits set.
			       XXX not perfect, might still fail */
			    S_ISREG(statbuf.st_mode) &&
			    (statbuf.st_mode & 0111))
				break;
		}
	}

  // pgbovine - if we still haven't initialized it yet, do so now
  if (path_to_search[0] == '\0') { // uninit
    if (CDE_exec_mode) {
      strcpy_redirected_cderoot(path_to_search, pathname);
    }
    else {
      strcpy(path_to_search, pathname);
    }
  }

	if (stat(path_to_search, &statbuf) < 0) {
		fprintf(stderr, "%s: %s: command not found (path_to_search=%s)\n",
			progname, filename, path_to_search);
		exit(1);
	}
	strace_child = pid = fork();
	if (pid < 0) {
		perror("strace: fork");
		cleanup();
		exit(1);
	}
	if ((pid != 0 && daemonized_tracer) /* parent: to become a traced process */
	 || (pid == 0 && !daemonized_tracer) /* child: to become a traced process */
	) {
		pid = getpid();
#ifdef USE_PROCFS
		if (outf != stderr) close (fileno (outf));
#ifdef MIPS
		/* Kludge for SGI, see proc_open for details. */
		sa.sa_handler = foobar;
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
#endif /* MIPS */
#ifndef FREEBSD
		pause();
#else /* FREEBSD */
		kill(pid, SIGSTOP); /* stop HERE */
#endif /* FREEBSD */
#else /* !USE_PROCFS */
		if (outf!=stderr)
			close(fileno (outf));

		if (!daemonized_tracer) {
			if (ptrace(PTRACE_TRACEME, 0, (char *) 1, 0) < 0) {
				perror("strace: ptrace(PTRACE_TRACEME, ...)");
				exit(1);
			}
			if (debug)
				kill(pid, SIGSTOP);
		}

		if (username != NULL || geteuid() == 0) {
			uid_t run_euid = run_uid;
			gid_t run_egid = run_gid;

			if (statbuf.st_mode & S_ISUID)
				run_euid = statbuf.st_uid;
			if (statbuf.st_mode & S_ISGID)
				run_egid = statbuf.st_gid;

			/*
			 * It is important to set groups before we
			 * lose privileges on setuid.
			 */
			if (username != NULL) {
				if (initgroups(username, run_gid) < 0) {
					perror("initgroups");
					exit(1);
				}
				if (setregid(run_gid, run_egid) < 0) {
					perror("setregid");
					exit(1);
				}
				if (setreuid(run_uid, run_euid) < 0) {
					perror("setreuid");
					exit(1);
				}
			}
		}
		else
			setreuid(run_uid, run_uid);

		if (!daemonized_tracer) {
			/*
			 * Induce an immediate stop so that the parent
			 * will resume us with PTRACE_SYSCALL and display
			 * this execve call normally.
			 * Unless of course we're on a no-MMU system where
			 * we vfork()-ed, so we cannot stop the child.
			 */
			if (!strace_vforked)
				kill(getpid(), SIGSTOP);
		} else {
			struct sigaction sv_sigchld;
			sigaction(SIGCHLD, NULL, &sv_sigchld);
			/*
			 * Make sure it is not SIG_IGN, otherwise wait
			 * will not block.
			 */
			signal(SIGCHLD, SIG_DFL);
			/*
			 * Wait for grandchild to attach to us.
			 * It kills child after that, and wait() unblocks.
			 */
			alarm(3);
			wait(NULL);
			alarm(0);
			sigaction(SIGCHLD, &sv_sigchld, NULL);
		}
#endif /* !USE_PROCFS */

    // pgbovine - subtle ... even though we look for the existence of
    // path_to_search, we still want to execute pathname, since our
    // CDE_begin_execve handler expects an original pristine pathname :)
    //printf("execv %s (path_to_search %s)\n", pathname, path_to_search);
		execv(pathname, argv);
		perror("strace: exec");
		_exit(1);
	}

	/* We are the tracer.  */
	tcp = alloctcb(daemonized_tracer ? getppid() : pid);
  CDE_init_tcb_dir_fields(tcp); // pgbovine

	if (daemonized_tracer) {
		/* We want subsequent startup_attach() to attach to it.  */
		tcp->flags |= TCB_ATTACHED;
	}
#ifdef USE_PROCFS
	if (proc_open(tcp, 0) < 0) {
		fprintf(stderr, "trouble opening proc file\n");
		cleanup();
		exit(1);
	}
#endif /* USE_PROCFS */
}

int
main(int argc, char *argv[])
{
	struct tcb *tcp;
	int c, pid = 0;
	int optF = 0;
	struct sigaction sa;

	static char buf[BUFSIZ];

  // pgbovine - make sure this constant is a reasonable number and not something KRAZY
  if (MAXPATHLEN > (1024 * 4096)) {
    fprintf(stderr, "cde error, MAXPATHLEN is HUGE!!!\n");
    exit(1);
  }

  if (!argv[0]) {
    fprintf(stderr, "cde error, wha???\n");
    exit(1);
  }
	progname = argv[0];

  // pgbovine - if program name is 'cde-exec', then activate CDE_exec_mode
  CDE_exec_mode = (strcmp(basename(progname), "cde-exec") == 0);


	/* Allocate the initial tcbtab.  */
	tcbtabsize = argc;	/* Surely enough for all -p args.  */
	if ((tcbtab = calloc(tcbtabsize, sizeof tcbtab[0])) == NULL) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}
	if ((tcbtab[0] = calloc(tcbtabsize, sizeof tcbtab[0][0])) == NULL) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}
	for (tcp = tcbtab[0]; tcp < &tcbtab[0][tcbtabsize]; ++tcp)
		tcbtab[tcp - tcbtab[0]] = &tcbtab[0][tcp - tcbtab[0]];

	outf = stderr;

  // pgbovine - set interactive to 0 by default (rather than 1) so that we
  // pass signals (e.g., SIGINT caused by Ctrl-C ) through to the child process
	//interactive = 1;
	interactive = 0;
 
	set_sortby(DEFAULT_SORTBY);
	set_personality(DEFAULT_PERSONALITY);

  // pgbovine - only track selected system calls
  // qualify actually mutates this string, so we can't pass in a constant
  //
  // syscalls added after Jan 1, 2011:
  //   utimes,openat,faccessat,fstatat64,fchownat,fchmodat,futimesat,mknodat
  //   linkat,symlinkat,renameat,readlinkat,mkdirat,unlinkat
  //   exit_group (only for provenance mode)
  char* tmp = strdup("trace=open,execve,stat,stat64,lstat,lstat64,oldstat,oldlstat,link,symlink,unlink,rename,access,creat,chmod,chown,chown32,lchown,lchown32,readlink,utime,truncate,truncate64,chdir,fchdir,mkdir,rmdir,getcwd,mknod,bind,connect,utimes,openat,faccessat,fstatat64,fchownat,fchmodat,futimesat,mknodat,linkat,symlinkat,renameat,readlinkat,mkdirat,unlinkat,exit_group");
	qualify(tmp);
  free(tmp);

	qualify("abbrev=all");
	qualify("verbose=all");
	qualify("signal=all");
	while ((c = getopt(argc, argv,
		"+cCdfFhqrtTvVxzpls"
#ifndef USE_PROCFS
		"D"
#endif
		"a:A:e:o:O:S:u:E:i:I:")) != EOF) {
		switch (c) {
		case 'c':
      // pgbovine - hijack for -c option
      CDE_copied_files_logfile = fopen("cde-copied-files.log", "w");

    /*
			if (cflag == CFLAG_BOTH) {
				fprintf(stderr, "%s: -c and -C are mutually exclusive options\n",
					progname);
				exit(1);
			}
			cflag = CFLAG_ONLY_STATS;
    */

			break;
		case 'C':
			if (cflag == CFLAG_ONLY_STATS) {
				fprintf(stderr, "%s: -c and -C are mutually exclusive options\n",
					progname);
				exit(1);
			}
			cflag = CFLAG_BOTH;
			break;
		case 'd':
			debug++;
			break;
#ifndef USE_PROCFS
		/* Experimental, not documented in manpage yet. */
		case 'D':
			daemonized_tracer = 1;
			break;
#endif
		case 'F':
			optF = 1;
			break;
		case 'f':
			followfork++;
			break;
		case 'h':
			usage(stdout, 0);
			break;
		case 'i':
			//iflag++;
			break;
		case 'I':
			break;
		case 'q':
			qflag++;
			break;
		case 'r':
			rflag++;
			tflag++;
			break;
		case 't':
			tflag++;
			break;
		case 'T':
			dtime++;
			break;
		case 'x':
			xflag++;
			break;
		case 'v':
      // pgbovine - hijack for the '-v' option
			//qualify("abbrev=none");
      CDE_verbose_mode = 1;
			break;
		case 'V':
			printf("%s -- version %s\n", PACKAGE_NAME, VERSION);
			exit(0);
			break;
		case 'z':
			not_failing_only = 1;
			break;
		case 'a':
			break;
		case 'A':
      break;
		case 'e':
			qualify(optarg);
			break;
		case 'o':
      // pgbovine - hijack for the '-o' option
      CDE_PACKAGE_DIR = strdup(optarg);
			//outfname = strdup(optarg);
			break;
		case 'O':
			set_overhead(atoi(optarg));
			break;
		case 'l':
      // pgbovine - hijack for the '-l' option
      CDE_use_linker_from_package = 0;
      break;
		case 'p':
      // pgbovine - hijack for the '-p' option
      CDE_provenance_mode = 1;
      extern FILE* CDE_provenance_logfile;
      CDE_provenance_logfile = fopen("provenance.log", "w");

      /*
			if ((pid = atoi(optarg)) <= 0) {
				fprintf(stderr, "%s: Invalid process id: %s\n",
					progname, optarg);
				break;
			}
			if (pid == getpid()) {
				fprintf(stderr, "%s: I'm sorry, I can't let you do that, Dave.\n", progname);
				break;
			}
			tcp = alloc_tcb(pid, 0);
      CDE_init_tcb_dir_fields(tcp); // pgbovine
			tcp->flags |= TCB_ATTACHED;
			pflag_seen++;
      */
			break;
		case 's':
      CDE_exec_streaming_mode = 1;
      // pgbovine - hijack for 's' (streaming mode)
      /*
			max_strlen = atoi(optarg);
			if (max_strlen < 0) {
				fprintf(stderr,
					"%s: invalid -s argument: %s\n",
					progname, optarg);
				exit(1);
			}
      */
			break;
		case 'S':
			set_sortby(optarg);
			break;
		case 'u':
			username = strdup(optarg);
			break;
		case 'E':
			break;
		default:
			usage(stderr, 1);
			break;
		}
	}

	if ((optind == argc) == !pflag_seen)
		usage(stderr, 1);


	if (!followfork)
		followfork = optF;

	if (followfork > 1 && cflag) {
		fprintf(stderr,
			"%s: (-c or -C) and -ff are mutually exclusive options\n",
			progname);
		exit(1);
	}

	/* See if they want to run as another user. */
	if (username != NULL) {
		struct passwd *pent;

		if (getuid() != 0 || geteuid() != 0) {
			fprintf(stderr,
				"%s: you must be root to use the -u option\n",
				progname);
			exit(1);
		}
		if ((pent = getpwnam(username)) == NULL) {
			fprintf(stderr, "%s: cannot find user `%s'\n",
				progname, username);
			exit(1);
		}
		run_uid = pent->pw_uid;
		run_gid = pent->pw_gid;
	}
	else {
		run_uid = getuid();
		run_gid = getgid();
	}

	/* Check if they want to redirect the output. */
	if (outfname) {
		/* See if they want to pipe the output. */
		if (outfname[0] == '|' || outfname[0] == '!') {
			/*
			 * We can't do the <outfname>.PID funny business
			 * when using popen, so prohibit it.
			 */
			if (followfork > 1) {
				fprintf(stderr, "\
%s: piping the output and -ff are mutually exclusive options\n",
					progname);
				exit(1);
			}

			if ((outf = strace_popen(outfname + 1)) == NULL)
				exit(1);
		}
		else if (followfork <= 1 &&
			 (outf = strace_fopen(outfname, "w")) == NULL)
			exit(1);
	}

	if (!outfname || outfname[0] == '|' || outfname[0] == '!')
		setvbuf(outf, buf, _IOLBF, BUFSIZ);
	if (outfname && optind < argc) {
		interactive = 0;
		qflag = 1;
	}
	/* Valid states here:
	   optind < argc	pflag_seen	outfname	interactive
	   1			0		0		1
	   0			1		0		1
	   1			0		1		0
	   0			1		1		1
	 */


  // pgbovine - do all CDE initialization here after command-line options
  // have been processed (argv[optind] is the name of the target program)

  // pgbovine - initialize this before doing anything else!
  getcwd(cde_starting_pwd, sizeof cde_starting_pwd);


  // pgbovine - allow most promiscuous permissions for new files/directories
  umask(0000);


  if (CDE_exec_mode) {
    // must do this before running CDE_init_options()
    CDE_exec_mode_early_init();
  }
  else {
    if (!CDE_PACKAGE_DIR) { // if it hasn't been set by the '-o' option, set to a default
      CDE_PACKAGE_DIR = "cde-package";
    }

    // make this an absolute path!
    CDE_PACKAGE_DIR = canonicalize_path(CDE_PACKAGE_DIR, cde_starting_pwd);
    CDE_ROOT_DIR = format("%s/%s", CDE_PACKAGE_DIR, CDE_ROOT_NAME);
    assert(IS_ABSPATH(CDE_ROOT_DIR));

    mkdir(CDE_PACKAGE_DIR, 0777);
    mkdir(CDE_ROOT_DIR, 0777);

    //printf("CDE_PACKAGE_DIR: %s\n", CDE_PACKAGE_DIR);
    //printf("CDE_ROOT_DIR: %s\n", CDE_ROOT_DIR);


    // if we can't even create CDE_ROOT_DIR, then abort with a failure
    struct stat cde_rootdir_stat;
    if (stat(CDE_ROOT_DIR, &cde_rootdir_stat)) {
      fprintf(stderr, "Error: Cannot create CDE root directory at \"%s\"\n", CDE_ROOT_DIR);
      exit(1);
    }


    // collect uname information in CDE_PACKAGE_DIR/cde.uname
    struct utsname uname_info;
    if (uname(&uname_info) >= 0) {
      char* fn = format("%s/cde.uname", CDE_PACKAGE_DIR);
      FILE* uname_f = fopen(fn, "w");
      free(fn);
      if (uname_f) {
        fprintf(uname_f, "uname: '%s' '%s' '%s' '%s'\n",
                          uname_info.sysname,
                          uname_info.release,
                          uname_info.version,
                          uname_info.machine);
        fclose(uname_f);
      }
    }

    // if cde.options doesn't yet exist, create it in pwd and seed it
    // with default values that are useful to ignore in practice
    //
    // do this BEFORE CDE_init_options() so that we pick up those
    // ignored values
    struct stat cde_options_stat;
    if (stat("cde.options", &cde_options_stat)) {
      FILE* f = fopen("cde.options", "w");

      fputs(CDE_OPTIONS_VERSION_NUM, f);
      fputs(" (do not alter this first line!)\n", f);

      // /dev, /proc, and /sys are special system directories with fake files
      //
      // some sub-directories within /var contains 'volatile' temp files
      // that change when system is running normally
      //
      // (Note that it's a bit too much to simply ignore all of /var,
      // since files in dirs like /var/lib might be required - e.g., see
      // gnome-sudoku example)
      //
      // $HOME/.Xauthority is used for X11 authentication via ssh, so we need to
      // use the REAL version and not the one in cde-root/
      //
      // ignore "/tmp" and "/tmp/*" since programs often put lots of
      // session-specific stuff into /tmp so DO NOT track files within
      // there, or else you will risk severely 'overfitting' and ruining
      // portability across machines.  it's safe to assume that all Linux
      // distros have a /tmp directory that anybody can write into
      fputs("\n# These directories often contain pseudo-files that shouldn't be tracked\n", f);
      fputs("ignore_prefix=/dev/\n", f);
      fputs("ignore_exact=/dev\n", f);
      fputs("ignore_prefix=/proc/\n", f);
      fputs("ignore_exact=/proc\n", f);
      fputs("ignore_prefix=/sys/\n", f);
      fputs("ignore_exact=/sys\n", f);
      fputs("ignore_prefix=/var/cache/\n", f);
      fputs("ignore_prefix=/var/lock/\n", f);
      fputs("ignore_prefix=/var/log/\n", f);
      fputs("ignore_prefix=/var/run/\n", f);
      fputs("ignore_prefix=/var/tmp/\n", f);
      fputs("ignore_prefix=/tmp/\n", f);
      fputs("ignore_exact=/tmp\n", f);

      fputs("\n# un-comment the entries below if you think they might help your app:\n", f);
      fputs("#ignore_exact=/etc/ld.so.cache\n", f);
      fputs("#ignore_exact=/etc/ld.so.preload\n", f);
      fputs("#ignore_exact=/etc/ld.so.nohwcap\n", f);

      fputs("\n# Ignore .Xauthority to allow X Windows programs to work\n", f);
      fputs("ignore_substr=.Xauthority\n", f);

      // we gotta ignore /etc/resolv.conf or else Google Earth can't
      // access the network when on another machine, so it won't work
      // (and I think other network-facing apps might not work either!)
      fputs("\n# Ignore so that networking can work properly\n", f);
      fputs("ignore_exact=/etc/resolv.conf\n", f);

      fputs("# These files might be useful to ignore along with /etc/resolv.conf\n", f);
      fputs("# (un-comment if you want to try them)\n", f);
      fputs("#ignore_exact=/etc/host.conf\n", f);
      fputs("#ignore_exact=/etc/hosts\n", f);
      fputs("#ignore_exact=/etc/nsswitch.conf\n", f);
      fputs("#ignore_exact=/etc/gai.conf\n", f);

      // ewencp also suggests looking into ignoring these other
      // networking-related files:
      /* Hmm, good point. There's probably lots -- if you're trying to
         run a server, /etc/hostname, /etc/hosts.allow and
         /etc/hosts.deny could all be problematic.  /etc/hosts could be
         a problem for client or server, although its unusual to have
         much in there. One way it could definitely be a problem is if
         the hostname is in /etc/hosts and you want to use it as a
         server, e.g. I run on my machine (ahoy) the server and client,
         which appears in /etc/hosts, and then when cde-exec runs it
         ends up returning 127.0.0.1.  But for all of these, I actually
         don't know when the file gets read, so I'm not certain any of
         them are really a problem. */

      fputs("\n# Access the target machine's password files:\n", f);
      fputs("# (some programs like texmacs need these lines to be commented-out,\n", f);
      fputs("#  since they try to use home directory paths within the passwd file,\n", f);
      fputs("#  and those paths might not exist within the package.)\n", f);
      fputs("ignore_prefix=/etc/passwd\n", f);
      fputs("ignore_prefix=/etc/shadow\n", f);


      fputs("\n# These environment vars might lead to 'overfitting' and hinder portability\n", f);
      fputs("ignore_environment_var=DBUS_SESSION_BUS_ADDRESS\n", f);
      fputs("ignore_environment_var=ORBIT_SOCKETDIR\n", f);
      fputs("ignore_environment_var=SESSION_MANAGER\n", f);
      fputs("ignore_environment_var=XAUTHORITY\n", f);
      fputs("ignore_environment_var=DISPLAY\n", f);
     
      fclose(f);
    }
  }


  // do this AFTER creating cde.options
  CDE_init_options();


  if (CDE_exec_mode) {
    CDE_load_environment_vars();
  }
  else {
    // pgbovine - copy 'cde' executable to CDE_PACKAGE_DIR and rename
    // it 'cde-exec', so that it can be included in the executable
    //
    // use /proc/self/exe since argv[0] might be simply 'cde'
    // (if the cde binary is in $PATH and we're invoking it only by its name)
    char* fn = format("%s/cde-exec", CDE_PACKAGE_DIR);
    copy_file("/proc/self/exe", fn, 0777);
    free(fn);

    CDE_create_convenience_scripts(argv, optind);


    // make a cde.log file that contains commands to reproduce original
    // run within cde-package
    struct stat tmp;
    FILE* log_f;
    char* log_filename = format("%s/cde.log", CDE_PACKAGE_DIR);
    if (stat(log_filename, &tmp)) {
      log_f = fopen(log_filename, "w");
      fprintf(log_f, "cd '" CDE_ROOT_NAME "%s'", cde_starting_pwd);
      fputc('\n', log_f);
    }
    else {
      log_f = fopen(log_filename, "a");
    }
    free(log_filename);

    fprintf(log_f, "'./%s.cde'", basename(argv[optind]));
    int i;
    for (i = optind + 1; argv[i] != NULL; i++) {
      fprintf(log_f, " '%s'", argv[i]); // add quotes for accuracy
    }
    fputc('\n', log_f);
    fclose(log_f);

    CDE_create_path_symlink_dirs();

    CDE_create_toplevel_symlink_dirs();


    // copy /proc/self/environ to capture the FULL set of environment vars
    char* fullenviron_fn = format("%s/cde.full-environment", CDE_PACKAGE_DIR);
    copy_file("/proc/self/environ", fullenviron_fn, 0666);
    free(fullenviron_fn);
  }


	/* STARTUP_CHILD must be called before the signal handlers get
	   installed below as they are inherited into the spawned process.
	   Also we do not need to be protected by them as during interruption
	   in the STARTUP_CHILD mode we kill the spawned process anyway.  */
	if (!pflag_seen)
		startup_child(&argv[optind]);

	sigemptyset(&empty_set);
	sigemptyset(&blocked_set);
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTTOU, &sa, NULL);
	sigaction(SIGTTIN, &sa, NULL);
	if (interactive) {
		sigaddset(&blocked_set, SIGHUP);
		sigaddset(&blocked_set, SIGINT);
		sigaddset(&blocked_set, SIGQUIT);
		sigaddset(&blocked_set, SIGPIPE);
		sigaddset(&blocked_set, SIGTERM);
		sa.sa_handler = interrupt;
#ifdef SUNOS4
		/* POSIX signals on sunos4.1 are a little broken. */
		sa.sa_flags = SA_INTERRUPT;
#endif /* SUNOS4 */
	}
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
#ifdef USE_PROCFS
	sa.sa_handler = reaper;
	sigaction(SIGCHLD, &sa, NULL);
#else
	/* Make sure SIGCHLD has the default action so that waitpid
	   definitely works without losing track of children.  The user
	   should not have given us a bogus state to inherit, but he might
	   have.  Arguably we should detect SIG_IGN here and pass it on
	   to children, but probably noone really needs that.  */
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
#endif /* USE_PROCFS */

	if (pflag_seen || daemonized_tracer)
		startup_attach();

	if (trace() < 0)
		exit(1);
	cleanup();
	fflush(NULL);
	if (exit_code > 0xff) {
		/* Child was killed by a signal, mimic that.  */
		exit_code &= 0xff;
		signal(exit_code, SIG_DFL);
		raise(exit_code);
		/* Paranoia - what if this signal is not fatal?
		   Exit with 128 + signo then.  */
		exit_code += 128;
	}
	exit(exit_code);
}

void
expand_tcbtab(void)
{
	/* Allocate some more TCBs and expand the table.
	   We don't want to relocate the TCBs because our
	   callers have pointers and it would be a pain.
	   So tcbtab is a table of pointers.  Since we never
	   free the TCBs, we allocate a single chunk of many.  */
	struct tcb **newtab = (struct tcb **)
		realloc(tcbtab, 2 * tcbtabsize * sizeof tcbtab[0]);
	struct tcb *newtcbs = (struct tcb *) calloc(tcbtabsize,
						    sizeof *newtcbs);
	int i;
	if (newtab == NULL || newtcbs == NULL) {
		fprintf(stderr, "%s: expand_tcbtab: out of memory\n",
			progname);
		cleanup();
		exit(1);
	}
	for (i = tcbtabsize; i < 2 * tcbtabsize; ++i)
		newtab[i] = &newtcbs[i - tcbtabsize];
	tcbtabsize *= 2;
	tcbtab = newtab;
}

struct tcb *
alloc_tcb(int pid, int command_options_parsed)
{
	int i;
	struct tcb *tcp;

	if (nprocs == tcbtabsize)
		expand_tcbtab();

	for (i = 0; i < tcbtabsize; i++) {
		tcp = tcbtab[i];
		if ((tcp->flags & TCB_INUSE) == 0) {
			tcp->pid = pid;
			tcp->parent = NULL;
			tcp->nchildren = 0;
			tcp->nzombies = 0;
#ifdef TCB_CLONE_THREAD
			tcp->nclone_threads = tcp->nclone_detached = 0;
			tcp->nclone_waiting = 0;
#endif
			tcp->flags = TCB_INUSE | TCB_STARTUP;
			tcp->outf = outf; /* Initialise to current out file */
			tcp->curcol = 0;
			tcp->stime.tv_sec = 0;
			tcp->stime.tv_usec = 0;
			tcp->pfd = -1;

      alloc_tcb_CDE_fields(tcp); // pgbovine

			nprocs++;
			if (command_options_parsed)
				newoutf(tcp);
			return tcp;
		}
	}
	fprintf(stderr, "%s: bug in alloc_tcb\n", progname);
	cleanup();
	exit(1);
}

#ifdef USE_PROCFS
int
proc_open(struct tcb *tcp, int attaching)
{
	char proc[32];
	long arg;
#ifdef SVR4
	int i;
	sysset_t syscalls;
	sigset_t signals;
	fltset_t faults;
#endif
#ifndef HAVE_POLLABLE_PROCFS
	static int last_pfd;
#endif

#ifdef HAVE_MP_PROCFS
	/* Open the process pseudo-files in /proc. */
	sprintf(proc, "/proc/%d/ctl", tcp->pid);
	if ((tcp->pfd = open(proc, O_WRONLY|O_EXCL)) < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	if (set_cloexec_flag(tcp->pfd) < 0) {
		return -1;
	}
	sprintf(proc, "/proc/%d/status", tcp->pid);
	if ((tcp->pfd_stat = open(proc, O_RDONLY|O_EXCL)) < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	if (set_cloexec_flag(tcp->pfd_stat) < 0) {
		return -1;
	}
	sprintf(proc, "/proc/%d/as", tcp->pid);
	if ((tcp->pfd_as = open(proc, O_RDONLY|O_EXCL)) < 0) {
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	if (set_cloexec_flag(tcp->pfd_as) < 0) {
		return -1;
	}
#else
	/* Open the process pseudo-file in /proc. */
#ifndef FREEBSD
	sprintf(proc, "/proc/%d", tcp->pid);
	if ((tcp->pfd = open(proc, O_RDWR|O_EXCL)) < 0) {
#else /* FREEBSD */
	sprintf(proc, "/proc/%d/mem", tcp->pid);
	if ((tcp->pfd = open(proc, O_RDWR)) < 0) {
#endif /* FREEBSD */
		perror("strace: open(\"/proc/...\", ...)");
		return -1;
	}
	if (set_cloexec_flag(tcp->pfd) < 0) {
		return -1;
	}
#endif
#ifdef FREEBSD
	sprintf(proc, "/proc/%d/regs", tcp->pid);
	if ((tcp->pfd_reg = open(proc, O_RDONLY)) < 0) {
		perror("strace: open(\"/proc/.../regs\", ...)");
		return -1;
	}
	if (cflag) {
		sprintf(proc, "/proc/%d/status", tcp->pid);
		if ((tcp->pfd_status = open(proc, O_RDONLY)) < 0) {
			perror("strace: open(\"/proc/.../status\", ...)");
			return -1;
		}
	} else
		tcp->pfd_status = -1;
#endif /* FREEBSD */
	rebuild_pollv();
	if (!attaching) {
		/*
		 * Wait for the child to pause.  Because of a race
		 * condition we have to poll for the event.
		 */
		for (;;) {
			if (IOCTL_STATUS (tcp) < 0) {
				perror("strace: PIOCSTATUS");
				return -1;
			}
			if (tcp->status.PR_FLAGS & PR_ASLEEP)
			    break;
		}
	}
#ifndef FREEBSD
	/* Stop the process so that we own the stop. */
	if (IOCTL(tcp->pfd, PIOCSTOP, (char *)NULL) < 0) {
		perror("strace: PIOCSTOP");
		return -1;
	}
#endif
#ifdef PIOCSET
	/* Set Run-on-Last-Close. */
	arg = PR_RLC;
	if (IOCTL(tcp->pfd, PIOCSET, &arg) < 0) {
		perror("PIOCSET PR_RLC");
		return -1;
	}
	/* Set or Reset Inherit-on-Fork. */
	arg = PR_FORK;
	if (IOCTL(tcp->pfd, followfork ? PIOCSET : PIOCRESET, &arg) < 0) {
		perror("PIOC{SET,RESET} PR_FORK");
		return -1;
	}
#else  /* !PIOCSET */
#ifndef FREEBSD
	if (ioctl(tcp->pfd, PIOCSRLC) < 0) {
		perror("PIOCSRLC");
		return -1;
	}
	if (ioctl(tcp->pfd, followfork ? PIOCSFORK : PIOCRFORK) < 0) {
		perror("PIOC{S,R}FORK");
		return -1;
	}
#else /* FREEBSD */
	/* just unset the PF_LINGER flag for the Run-on-Last-Close. */
	if (ioctl(tcp->pfd, PIOCGFL, &arg) < 0) {
	        perror("PIOCGFL");
		return -1;
	}
	arg &= ~PF_LINGER;
	if (ioctl(tcp->pfd, PIOCSFL, arg) < 0) {
		perror("PIOCSFL");
		return -1;
	}
#endif /* FREEBSD */
#endif /* !PIOCSET */
#ifndef FREEBSD
	/* Enable all syscall entries we care about. */
	premptyset(&syscalls);
	for (i = 1; i < MAX_QUALS; ++i) {
		if (i > (sizeof syscalls) * CHAR_BIT) break;
		if (qual_flags [i] & QUAL_TRACE) praddset (&syscalls, i);
	}
	praddset (&syscalls, SYS_execve);
	if (followfork) {
		praddset (&syscalls, SYS_fork);
#ifdef SYS_forkall
		praddset (&syscalls, SYS_forkall);
#endif
#ifdef SYS_fork1
		praddset (&syscalls, SYS_fork1);
#endif
#ifdef SYS_rfork1
		praddset (&syscalls, SYS_rfork1);
#endif
#ifdef SYS_rforkall
		praddset (&syscalls, SYS_rforkall);
#endif
	}
	if (IOCTL(tcp->pfd, PIOCSENTRY, &syscalls) < 0) {
		perror("PIOCSENTRY");
		return -1;
	}
	/* Enable the syscall exits. */
	if (IOCTL(tcp->pfd, PIOCSEXIT, &syscalls) < 0) {
		perror("PIOSEXIT");
		return -1;
	}
	/* Enable signals we care about. */
	premptyset(&signals);
	for (i = 1; i < MAX_QUALS; ++i) {
		if (i > (sizeof signals) * CHAR_BIT) break;
		if (qual_flags [i] & QUAL_SIGNAL) praddset (&signals, i);
	}
	if (IOCTL(tcp->pfd, PIOCSTRACE, &signals) < 0) {
		perror("PIOCSTRACE");
		return -1;
	}
	/* Enable faults we care about */
	premptyset(&faults);
	for (i = 1; i < MAX_QUALS; ++i) {
		if (i > (sizeof faults) * CHAR_BIT) break;
		if (qual_flags [i] & QUAL_FAULT) praddset (&faults, i);
	}
	if (IOCTL(tcp->pfd, PIOCSFAULT, &faults) < 0) {
		perror("PIOCSFAULT");
		return -1;
	}
#else /* FREEBSD */
	/* set events flags. */
	arg = S_SIG | S_SCE | S_SCX ;
	if(ioctl(tcp->pfd, PIOCBIS, arg) < 0) {
		perror("PIOCBIS");
		return -1;
	}
#endif /* FREEBSD */
	if (!attaching) {
#ifdef MIPS
		/*
		 * The SGI PRSABORT doesn't work for pause() so
		 * we send it a caught signal to wake it up.
		 */
		kill(tcp->pid, SIGINT);
#else /* !MIPS */
#ifdef PRSABORT
		/* The child is in a pause(), abort it. */
		arg = PRSABORT;
		if (IOCTL (tcp->pfd, PIOCRUN, &arg) < 0) {
			perror("PIOCRUN");
			return -1;
		}
#endif
#endif /* !MIPS*/
#ifdef FREEBSD
		/* wake up the child if it received the SIGSTOP */
		kill(tcp->pid, SIGCONT);
#endif
		for (;;) {
			/* Wait for the child to do something. */
			if (IOCTL_WSTOP (tcp) < 0) {
				perror("PIOCWSTOP");
				return -1;
			}
			if (tcp->status.PR_WHY == PR_SYSENTRY) {
				tcp->flags &= ~TCB_INSYSCALL;
				get_scno(tcp);
				if (known_scno(tcp) == SYS_execve)
					break;
			}
			/* Set it running: maybe execve will be next. */
#ifndef FREEBSD
			arg = 0;
			if (IOCTL(tcp->pfd, PIOCRUN, &arg) < 0) {
#else /* FREEBSD */
			if (IOCTL(tcp->pfd, PIOCRUN, 0) < 0) {
#endif /* FREEBSD */
				perror("PIOCRUN");
				return -1;
			}
#ifdef FREEBSD
			/* handle the case where we "opened" the child before
			   it did the kill -STOP */
			if (tcp->status.PR_WHY == PR_SIGNALLED &&
			    tcp->status.PR_WHAT == SIGSTOP)
			        kill(tcp->pid, SIGCONT);
#endif
		}
#ifndef FREEBSD
	}
#else /* FREEBSD */
	} else {
		if (attaching < 2) {
			/* We are attaching to an already running process.
			 * Try to figure out the state of the process in syscalls,
			 * to handle the first event well.
			 * This is done by having a look at the "wchan" property of the
			 * process, which tells where it is stopped (if it is). */
			FILE * status;
			char wchan[20]; /* should be enough */

			sprintf(proc, "/proc/%d/status", tcp->pid);
			status = fopen(proc, "r");
			if (status &&
			    (fscanf(status, "%*s %*d %*d %*d %*d %*d,%*d %*s %*d,%*d"
				    "%*d,%*d %*d,%*d %19s", wchan) == 1) &&
			    strcmp(wchan, "nochan") && strcmp(wchan, "spread") &&
			    strcmp(wchan, "stopevent")) {
				/* The process is asleep in the middle of a syscall.
				   Fake the syscall entry event */
				tcp->flags &= ~(TCB_INSYSCALL|TCB_STARTUP);
				tcp->status.PR_WHY = PR_SYSENTRY;
				trace_syscall(tcp);
			}
			if (status)
				fclose(status);
		} /* otherwise it's a fork being followed */
	}
#endif /* FREEBSD */
#ifndef HAVE_POLLABLE_PROCFS
	if (proc_poll_pipe[0] != -1)
		proc_poller(tcp->pfd);
	else if (nprocs > 1) {
		proc_poll_open();
		proc_poller(last_pfd);
		proc_poller(tcp->pfd);
	}
	last_pfd = tcp->pfd;
#endif /* !HAVE_POLLABLE_PROCFS */
	return 0;
}

#endif /* USE_PROCFS */

struct tcb *
pid2tcb(pid)
int pid;
{
	int i;
	struct tcb *tcp;

	for (i = 0; i < tcbtabsize; i++) {
		tcp = tcbtab[i];
		if (pid && tcp->pid != pid)
			continue;
		if (tcp->flags & TCB_INUSE)
			return tcp;
	}
	return NULL;
}

#ifdef USE_PROCFS

static struct tcb *
pfd2tcb(pfd)
int pfd;
{
	int i;

	for (i = 0; i < tcbtabsize; i++) {
		struct tcb *tcp = tcbtab[i];
		if (tcp->pfd != pfd)
			continue;
		if (tcp->flags & TCB_INUSE)
			return tcp;
	}
	return NULL;
}

#endif /* USE_PROCFS */

void
droptcb(tcp)
struct tcb *tcp;
{
	if (tcp->pid == 0)
		return;
#ifdef TCB_CLONE_THREAD
	if (tcp->nclone_threads > 0) {
		/* There are other threads left in this process, but this
		   is the one whose PID represents the whole process.
		   We need to keep this record around as a zombie until
		   all the threads die.  */
		tcp->flags |= TCB_EXITING;
		return;
	}
#endif
	nprocs--;
	tcp->pid = 0;

	if (tcp->parent != NULL) {
		tcp->parent->nchildren--;
#ifdef TCB_CLONE_THREAD
		if (tcp->flags & TCB_CLONE_DETACHED)
			tcp->parent->nclone_detached--;
		if (tcp->flags & TCB_CLONE_THREAD)
			tcp->parent->nclone_threads--;
#endif
#ifdef TCB_CLONE_DETACHED
		if (!(tcp->flags & TCB_CLONE_DETACHED))
#endif
			tcp->parent->nzombies++;
#ifdef LINUX
		/* Update `tcp->parent->parent->nchildren' and the other fields
		   like NCLONE_DETACHED, only for zombie group leader that has
		   already reported and been short-circuited at the top of this
		   function.  The same condition as at the top of DETACH.  */
		if ((tcp->flags & TCB_CLONE_THREAD) &&
		    tcp->parent->nclone_threads == 0 &&
		    (tcp->parent->flags & TCB_EXITING))
			droptcb(tcp->parent);
#endif
		tcp->parent = NULL;
	}

	tcp->flags = 0;
	if (tcp->pfd != -1) {
		close(tcp->pfd);
		tcp->pfd = -1;
#ifdef FREEBSD
		if (tcp->pfd_reg != -1) {
		        close(tcp->pfd_reg);
		        tcp->pfd_reg = -1;
		}
		if (tcp->pfd_status != -1) {
			close(tcp->pfd_status);
			tcp->pfd_status = -1;
		}
#endif /* !FREEBSD */
#ifdef USE_PROCFS
		rebuild_pollv(); /* Note, flags needs to be cleared by now.  */
#endif
	}

	if (outfname && followfork > 1 && tcp->outf)
		fclose(tcp->outf);

	tcp->outf = 0;

  free_tcb_CDE_fields(tcp); // pgbovine
}

#ifndef USE_PROCFS

static int
resume(tcp)
struct tcb *tcp;
{
	if (tcp == NULL)
		return -1;

	if (!(tcp->flags & TCB_SUSPENDED)) {
		fprintf(stderr, "PANIC: pid %u not suspended\n", tcp->pid);
		return -1;
	}
	tcp->flags &= ~TCB_SUSPENDED;
#ifdef TCB_CLONE_THREAD
	if (tcp->flags & TCB_CLONE_THREAD)
		tcp->parent->nclone_waiting--;
#endif

	if (ptrace_restart(PTRACE_SYSCALL, tcp, 0) < 0)
		return -1;

	if (!qflag)
		fprintf(stderr, "Process %u resumed\n", tcp->pid);
	return 0;
}

static int
resume_from_tcp (struct tcb *tcp)
{
	int error = 0;
	int resumed = 0;

	/* XXX This won't always be quite right (but it never was).
	   A waiter with argument 0 or < -1 is waiting for any pid in
	   a particular pgrp, which this child might or might not be
	   in.  The waiter will only wake up if it's argument is -1
	   or if it's waiting for tcp->pid's pgrp.  It makes a
	   difference to wake up a waiter when there might be more
	   traced children, because it could get a false ECHILD
	   error.  OTOH, if this was the last child in the pgrp, then
	   it ought to wake up and get ECHILD.  We would have to
	   search the system for all pid's in the pgrp to be sure.

	     && (t->waitpid == -1 ||
		 (t->waitpid == 0 && getpgid (tcp->pid) == getpgid (t->pid))
		 || (t->waitpid < 0 && t->waitpid == -getpid (t->pid)))
	*/

	if (tcp->parent &&
	    (tcp->parent->flags & TCB_SUSPENDED) &&
	    (tcp->parent->waitpid <= 0 || tcp->parent->waitpid == tcp->pid)) {
		error = resume(tcp->parent);
		++resumed;
	}
#ifdef TCB_CLONE_THREAD
	if (tcp->parent && tcp->parent->nclone_waiting > 0) {
		/* Some other threads of our parent are waiting too.  */
		unsigned int i;

		/* Resume all the threads that were waiting for this PID.  */
		for (i = 0; i < tcbtabsize; i++) {
			struct tcb *t = tcbtab[i];
			if (t->parent == tcp->parent && t != tcp
			    && ((t->flags & (TCB_CLONE_THREAD|TCB_SUSPENDED))
				== (TCB_CLONE_THREAD|TCB_SUSPENDED))
			    && t->waitpid == tcp->pid) {
				error |= resume (t);
				++resumed;
			}
		}
		if (resumed == 0)
			/* Noone was waiting for this PID in particular,
			   so now we might need to resume some wildcarders.  */
			for (i = 0; i < tcbtabsize; i++) {
				struct tcb *t = tcbtab[i];
				if (t->parent == tcp->parent && t != tcp
				    && ((t->flags
					 & (TCB_CLONE_THREAD|TCB_SUSPENDED))
					== (TCB_CLONE_THREAD|TCB_SUSPENDED))
				    && t->waitpid <= 0
					) {
					error |= resume (t);
					break;
				}
			}
	}
#endif

	return error;
}

#endif /* !USE_PROCFS */

/* detach traced process; continue with sig
   Never call DETACH twice on the same process as both unattached and
   attached-unstopped processes give the same ESRCH.  For unattached process we
   would SIGSTOP it and wait for its SIGSTOP notification forever.  */

static int
detach(tcp, sig)
struct tcb *tcp;
int sig;
{
	int error = 0;
#ifdef LINUX
	int status, catch_sigstop;
	struct tcb *zombie = NULL;

	/* If the group leader is lingering only because of this other
	   thread now dying, then detach the leader as well.  */
	if ((tcp->flags & TCB_CLONE_THREAD) &&
	    tcp->parent->nclone_threads == 1 &&
	    (tcp->parent->flags & TCB_EXITING))
		zombie = tcp->parent;
#endif

	if (tcp->flags & TCB_BPTSET)
		clearbpt(tcp);

#ifdef LINUX
	/*
	 * Linux wrongly insists the child be stopped
	 * before detaching.  Arghh.  We go through hoops
	 * to make a clean break of things.
	 */
#if defined(SPARC)
#undef PTRACE_DETACH
#define PTRACE_DETACH PTRACE_SUNDETACH
#endif
	/*
	 * On TCB_STARTUP we did PTRACE_ATTACH but still did not get the
	 * expected SIGSTOP.  We must catch exactly one as otherwise the
	 * detached process would be left stopped (process state T).
	 */
	catch_sigstop = (tcp->flags & TCB_STARTUP);
	if ((error = ptrace(PTRACE_DETACH, tcp->pid, (char *) 1, sig)) == 0) {
		/* On a clear day, you can see forever. */
	}
	else if (errno != ESRCH) {
		/* Shouldn't happen. */
		perror("detach: ptrace(PTRACE_DETACH, ...)");
	}
	else if (my_tgkill((tcp->flags & TCB_CLONE_THREAD ? tcp->parent->pid
							  : tcp->pid),
			   tcp->pid, 0) < 0) {
		if (errno != ESRCH)
			perror("detach: checking sanity");
	}
	else if (!catch_sigstop && my_tgkill((tcp->flags & TCB_CLONE_THREAD
					      ? tcp->parent->pid : tcp->pid),
					     tcp->pid, SIGSTOP) < 0) {
		if (errno != ESRCH)
			perror("detach: stopping child");
	}
	else
		catch_sigstop = 1;
	if (catch_sigstop) {
		for (;;) {
#ifdef __WALL
			if (wait4(tcp->pid, &status, __WALL, NULL) < 0) {
				if (errno == ECHILD) /* Already gone.  */
					break;
				if (errno != EINVAL) {
					perror("detach: waiting");
					break;
				}
#endif /* __WALL */
				/* No __WALL here.  */
				if (waitpid(tcp->pid, &status, 0) < 0) {
					if (errno != ECHILD) {
						perror("detach: waiting");
						break;
					}
#ifdef __WCLONE
					/* If no processes, try clones.  */
					if (wait4(tcp->pid, &status, __WCLONE,
						  NULL) < 0) {
						if (errno != ECHILD)
							perror("detach: waiting");
						break;
					}
#endif /* __WCLONE */
				}
#ifdef __WALL
			}
#endif
			if (!WIFSTOPPED(status)) {
				/* Au revoir, mon ami. */
				break;
			}
			if (WSTOPSIG(status) == SIGSTOP) {
				ptrace_restart(PTRACE_DETACH, tcp, sig);
				break;
			}
			error = ptrace_restart(PTRACE_CONT, tcp,
					WSTOPSIG(status) == SIGTRAP ? 0
					: WSTOPSIG(status));
			if (error < 0)
				break;
		}
	}
#endif /* LINUX */

#if defined(SUNOS4)
	/* PTRACE_DETACH won't respect `sig' argument, so we post it here. */
	if (sig && kill(tcp->pid, sig) < 0)
		perror("detach: kill");
	sig = 0;
	error = ptrace_restart(PTRACE_DETACH, tcp, sig);
#endif /* SUNOS4 */

#ifndef USE_PROCFS
	error |= resume_from_tcp (tcp);
#endif

	if (!qflag)
		fprintf(stderr, "Process %u detached\n", tcp->pid);

	droptcb(tcp);

#ifdef LINUX
	if (zombie != NULL) {
		/* TCP no longer exists therefore you must not detach () it.  */
		droptcb(zombie);
	}
#endif

	return error;
}

#ifdef USE_PROCFS

static void reaper(int sig)
{
	int pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
	}
}

#endif /* USE_PROCFS */

static void
cleanup()
{
	int i;
	struct tcb *tcp;

	for (i = 0; i < tcbtabsize; i++) {
		tcp = tcbtab[i];
		if (!(tcp->flags & TCB_INUSE))
			continue;
		if (debug)
			fprintf(stderr,
				"cleanup: looking at pid %u\n", tcp->pid);
		if (tcp_last &&
		    (!outfname || followfork < 2 || tcp_last == tcp)) {
			tprintf(" <unfinished ...>");
			printtrailer();
		}
		if (tcp->flags & TCB_ATTACHED)
			detach(tcp, 0);
		else {
			kill(tcp->pid, SIGCONT);
			kill(tcp->pid, SIGTERM);
		}
	}
	if (cflag)
		call_summary(outf);
}

static void
interrupt(sig)
int sig;
{
	interrupted = 1;
}

#ifndef HAVE_STRERROR

#if !HAVE_DECL_SYS_ERRLIST
extern int sys_nerr;
extern char *sys_errlist[];
#endif /* HAVE_DECL_SYS_ERRLIST */

const char *
strerror(errno)
int errno;
{
	static char buf[64];

	if (errno < 1 || errno >= sys_nerr) {
		sprintf(buf, "Unknown error %d", errno);
		return buf;
	}
	return sys_errlist[errno];
}

#endif /* HAVE_STERRROR */

#ifndef HAVE_STRSIGNAL

#if defined HAVE_SYS_SIGLIST && !defined HAVE_DECL_SYS_SIGLIST
extern char *sys_siglist[];
#endif
#if defined HAVE_SYS__SIGLIST && !defined HAVE_DECL__SYS_SIGLIST
extern char *_sys_siglist[];
#endif

const char *
strsignal(sig)
int sig;
{
	static char buf[64];

	if (sig < 1 || sig >= NSIG) {
		sprintf(buf, "Unknown signal %d", sig);
		return buf;
	}
#ifdef HAVE__SYS_SIGLIST
	return _sys_siglist[sig];
#else
	return sys_siglist[sig];
#endif
}

#endif /* HAVE_STRSIGNAL */

#ifdef USE_PROCFS

static void
rebuild_pollv()
{
	int i, j;

	if (pollv != NULL)
		free (pollv);
	pollv = (struct pollfd *) malloc(nprocs * sizeof pollv[0]);
	if (pollv == NULL) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}

	for (i = j = 0; i < tcbtabsize; i++) {
		struct tcb *tcp = tcbtab[i];
		if (!(tcp->flags & TCB_INUSE))
			continue;
		pollv[j].fd = tcp->pfd;
		pollv[j].events = POLLWANT;
		j++;
	}
	if (j != nprocs) {
		fprintf(stderr, "strace: proc miscount\n");
		exit(1);
	}
}

#ifndef HAVE_POLLABLE_PROCFS

static void
proc_poll_open()
{
	int i;

	if (pipe(proc_poll_pipe) < 0) {
		perror("pipe");
		exit(1);
	}
	for (i = 0; i < 2; i++) {
		if (set_cloexec_flag(proc_poll_pipe[i]) < 0) {
			exit(1);
		}
	}
}

static int
proc_poll(pollv, nfds, timeout)
struct pollfd *pollv;
int nfds;
int timeout;
{
	int i;
	int n;
	struct proc_pollfd pollinfo;

	if ((n = read(proc_poll_pipe[0], &pollinfo, sizeof(pollinfo))) < 0)
		return n;
	if (n != sizeof(struct proc_pollfd)) {
		fprintf(stderr, "panic: short read: %d\n", n);
		exit(1);
	}
	for (i = 0; i < nprocs; i++) {
		if (pollv[i].fd == pollinfo.fd)
			pollv[i].revents = pollinfo.revents;
		else
			pollv[i].revents = 0;
	}
	poller_pid = pollinfo.pid;
	return 1;
}

static void
wakeup_handler(sig)
int sig;
{
}

static void
proc_poller(pfd)
int pfd;
{
	struct proc_pollfd pollinfo;
	struct sigaction sa;
	sigset_t blocked_set, empty_set;
	int i;
	int n;
	struct rlimit rl;
#ifdef FREEBSD
	struct procfs_status pfs;
#endif /* FREEBSD */

	switch (fork()) {
	case -1:
		perror("fork");
		_exit(1);
	case 0:
		break;
	default:
		return;
	}

	sa.sa_handler = interactive ? SIG_DFL : SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = wakeup_handler;
	sigaction(SIGUSR1, &sa, NULL);
	sigemptyset(&blocked_set);
	sigaddset(&blocked_set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &blocked_set, NULL);
	sigemptyset(&empty_set);

	if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
		perror("getrlimit(RLIMIT_NOFILE, ...)");
		_exit(1);
	}
	n = rl.rlim_cur;
	for (i = 0; i < n; i++) {
		if (i != pfd && i != proc_poll_pipe[1])
			close(i);
	}

	pollinfo.fd = pfd;
	pollinfo.pid = getpid();
	for (;;) {
#ifndef FREEBSD
		if (ioctl(pfd, PIOCWSTOP, NULL) < 0)
#else
		if (ioctl(pfd, PIOCWSTOP, &pfs) < 0)
#endif
		{
			switch (errno) {
			case EINTR:
				continue;
			case EBADF:
				pollinfo.revents = POLLERR;
				break;
			case ENOENT:
				pollinfo.revents = POLLHUP;
				break;
			default:
				perror("proc_poller: PIOCWSTOP");
			}
			write(proc_poll_pipe[1], &pollinfo, sizeof(pollinfo));
			_exit(0);
		}
		pollinfo.revents = POLLWANT;
		write(proc_poll_pipe[1], &pollinfo, sizeof(pollinfo));
		sigsuspend(&empty_set);
	}
}

#endif /* !HAVE_POLLABLE_PROCFS */

static int
choose_pfd()
{
	int i, j;
	struct tcb *tcp;

	static int last;

	if (followfork < 2 &&
	    last < nprocs && (pollv[last].revents & POLLWANT)) {
		/*
		 * The previous process is ready to run again.  We'll
		 * let it do so if it is currently in a syscall.  This
		 * heuristic improves the readability of the trace.
		 */
		tcp = pfd2tcb(pollv[last].fd);
		if (tcp && (tcp->flags & TCB_INSYSCALL))
			return pollv[last].fd;
	}

	for (i = 0; i < nprocs; i++) {
		/* Let competing children run round robin. */
		j = (i + last + 1) % nprocs;
		if (pollv[j].revents & (POLLHUP | POLLERR)) {
			tcp = pfd2tcb(pollv[j].fd);
			if (!tcp) {
				fprintf(stderr, "strace: lost proc\n");
				exit(1);
			}
			droptcb(tcp);
			return -1;
		}
		if (pollv[j].revents & POLLWANT) {
			last = j;
			return pollv[j].fd;
		}
	}
	fprintf(stderr, "strace: nothing ready\n");
	exit(1);
}

static int
trace()
{
#ifdef POLL_HACK
	struct tcb *in_syscall = NULL;
#endif
	struct tcb *tcp;
	int pfd;
	int what;
	int ioctl_result = 0, ioctl_errno = 0;
	long arg;

	for (;;) {
		if (interactive)
			sigprocmask(SIG_SETMASK, &empty_set, NULL);

		if (nprocs == 0)
			break;

		switch (nprocs) {
		case 1:
#ifndef HAVE_POLLABLE_PROCFS
			if (proc_poll_pipe[0] == -1) {
#endif
				tcp = pid2tcb(0);
				if (!tcp)
					continue;
				pfd = tcp->pfd;
				if (pfd == -1)
					continue;
				break;
#ifndef HAVE_POLLABLE_PROCFS
			}
			/* fall through ... */
#endif /* !HAVE_POLLABLE_PROCFS */
		default:
#ifdef HAVE_POLLABLE_PROCFS
#ifdef POLL_HACK
		        /* On some systems (e.g. UnixWare) we get too much ugly
			   "unfinished..." stuff when multiple proceses are in
			   syscalls.  Here's a nasty hack */

			if (in_syscall) {
				struct pollfd pv;
				tcp = in_syscall;
				in_syscall = NULL;
				pv.fd = tcp->pfd;
				pv.events = POLLWANT;
				if ((what = poll (&pv, 1, 1)) < 0) {
					if (interrupted)
						return 0;
					continue;
				}
				else if (what == 1 && pv.revents & POLLWANT) {
					goto FOUND;
				}
			}
#endif

			if (poll(pollv, nprocs, INFTIM) < 0) {
				if (interrupted)
					return 0;
				continue;
			}
#else /* !HAVE_POLLABLE_PROCFS */
			if (proc_poll(pollv, nprocs, INFTIM) < 0) {
				if (interrupted)
					return 0;
				continue;
			}
#endif /* !HAVE_POLLABLE_PROCFS */
			pfd = choose_pfd();
			if (pfd == -1)
				continue;
			break;
		}

		/* Look up `pfd' in our table. */
		if ((tcp = pfd2tcb(pfd)) == NULL) {
			fprintf(stderr, "unknown pfd: %u\n", pfd);
			exit(1);
		}
#ifdef POLL_HACK
	FOUND:
#endif
		/* Get the status of the process. */
		if (!interrupted) {
#ifndef FREEBSD
			ioctl_result = IOCTL_WSTOP (tcp);
#else /* FREEBSD */
			/* Thanks to some scheduling mystery, the first poller
			   sometimes waits for the already processed end of fork
			   event. Doing a non blocking poll here solves the problem. */
			if (proc_poll_pipe[0] != -1)
				ioctl_result = IOCTL_STATUS (tcp);
			else
				ioctl_result = IOCTL_WSTOP (tcp);
#endif /* FREEBSD */
			ioctl_errno = errno;
#ifndef HAVE_POLLABLE_PROCFS
			if (proc_poll_pipe[0] != -1) {
				if (ioctl_result < 0)
					kill(poller_pid, SIGKILL);
				else
					kill(poller_pid, SIGUSR1);
			}
#endif /* !HAVE_POLLABLE_PROCFS */
		}
		if (interrupted)
			return 0;

		if (interactive)
			sigprocmask(SIG_BLOCK, &blocked_set, NULL);

		if (ioctl_result < 0) {
			/* Find out what happened if it failed. */
			switch (ioctl_errno) {
			case EINTR:
			case EBADF:
				continue;
#ifdef FREEBSD
			case ENOTTY:
#endif
			case ENOENT:
				droptcb(tcp);
				continue;
			default:
				perror("PIOCWSTOP");
				exit(1);
			}
		}

#ifdef FREEBSD
		if ((tcp->flags & TCB_STARTUP) && (tcp->status.PR_WHY == PR_SYSEXIT)) {
			/* discard first event for a syscall we never entered */
			IOCTL (tcp->pfd, PIOCRUN, 0);
			continue;
		}
#endif

		/* clear the just started flag */
		tcp->flags &= ~TCB_STARTUP;

		/* set current output file */
		outf = tcp->outf;
		curcol = tcp->curcol;

		if (cflag) {
			struct timeval stime;
#ifdef FREEBSD
			char buf[1024];
			int len;

			if ((len = pread(tcp->pfd_status, buf, sizeof(buf) - 1, 0)) > 0) {
				buf[len] = '\0';
				sscanf(buf,
				       "%*s %*d %*d %*d %*d %*d,%*d %*s %*d,%*d %*d,%*d %ld,%ld",
				       &stime.tv_sec, &stime.tv_usec);
			} else
				stime.tv_sec = stime.tv_usec = 0;
#else /* !FREEBSD */
			stime.tv_sec = tcp->status.pr_stime.tv_sec;
			stime.tv_usec = tcp->status.pr_stime.tv_nsec/1000;
#endif /* !FREEBSD */
			tv_sub(&tcp->dtime, &stime, &tcp->stime);
			tcp->stime = stime;
		}
		what = tcp->status.PR_WHAT;
		switch (tcp->status.PR_WHY) {
#ifndef FREEBSD
		case PR_REQUESTED:
			if (tcp->status.PR_FLAGS & PR_ASLEEP) {
				tcp->status.PR_WHY = PR_SYSENTRY;
				if (trace_syscall(tcp) < 0) {
					fprintf(stderr, "syscall trouble\n");
					exit(1);
				}
			}
			break;
#endif /* !FREEBSD */
		case PR_SYSENTRY:
#ifdef POLL_HACK
		        in_syscall = tcp;
#endif
		case PR_SYSEXIT:
			if (trace_syscall(tcp) < 0) {
				fprintf(stderr, "syscall trouble\n");
				exit(1);
			}
			break;
		case PR_SIGNALLED:
			if (cflag != CFLAG_ONLY_STATS
			    && (qual_flags[what] & QUAL_SIGNAL)) {
				printleader(tcp);
				tprintf("--- %s (%s) ---",
					signame(what), strsignal(what));
				printtrailer();
#ifdef PR_INFO
				if (tcp->status.PR_INFO.si_signo == what) {
					printleader(tcp);
					tprintf("    siginfo=");
					printsiginfo(&tcp->status.PR_INFO, 1);
					printtrailer();
				}
#endif
			}
			break;
		case PR_FAULTED:
			if (cflag != CFLAGS_ONLY_STATS
			    && (qual_flags[what] & QUAL_FAULT)) {
				printleader(tcp);
				tprintf("=== FAULT %d ===", what);
				printtrailer();
			}
			break;
#ifdef FREEBSD
		case 0: /* handle case we polled for nothing */
			continue;
#endif
		default:
			fprintf(stderr, "odd stop %d\n", tcp->status.PR_WHY);
			exit(1);
			break;
		}
		/* Remember current print column before continuing. */
		tcp->curcol = curcol;
		arg = 0;
#ifndef FREEBSD
		if (IOCTL (tcp->pfd, PIOCRUN, &arg) < 0) {
#else
		if (IOCTL (tcp->pfd, PIOCRUN, 0) < 0) {
#endif
			perror("PIOCRUN");
			exit(1);
		}
	}
	return 0;
}

#else /* !USE_PROCFS */

#ifdef TCB_GROUP_EXITING
/* Handle an exit detach or death signal that is taking all the
   related clone threads with it.  This is called in three circumstances:
   SIG == -1	TCP has already died (TCB_ATTACHED is clear, strace is parent).
   SIG == 0	Continuing TCP will perform an exit_group syscall.
   SIG == other	Continuing TCP with SIG will kill the process.
*/
static int
handle_group_exit(struct tcb *tcp, int sig)
{
	/* We need to locate our records of all the clone threads
	   related to TCP, either its children or siblings.  */
	struct tcb *leader = NULL;

	if (tcp->flags & TCB_CLONE_THREAD)
		leader = tcp->parent;
	else if (tcp->nclone_detached > 0)
		leader = tcp;

	if (sig < 0) {
		if (leader != NULL && leader != tcp
		 && !(leader->flags & TCB_GROUP_EXITING)
		 && !(tcp->flags & TCB_STARTUP)
		) {
			fprintf(stderr,
				"PANIC: handle_group_exit: %d leader %d\n",
				tcp->pid, leader ? leader->pid : -1);
		}
		/* TCP no longer exists therefore you must not detach() it.  */
#ifndef USE_PROCFS
		resume_from_tcp(tcp);
#endif
		droptcb(tcp);	/* Already died.  */
	}
	else {
		/* Mark that we are taking the process down.  */
		tcp->flags |= TCB_EXITING | TCB_GROUP_EXITING;
		if (tcp->flags & TCB_ATTACHED) {
			detach(tcp, sig);
			if (leader != NULL && leader != tcp)
				leader->flags |= TCB_GROUP_EXITING;
		} else {
			if (ptrace_restart(PTRACE_CONT, tcp, sig) < 0) {
				cleanup();
				return -1;
			}
			if (leader != NULL) {
				leader->flags |= TCB_GROUP_EXITING;
				if (leader != tcp)
					droptcb(tcp);
			}
			/* The leader will report to us as parent now,
			   and then we'll get to the SIG==-1 case.  */
			return 0;
		}
	}

	return 0;
}
#endif

static int
trace()
{
	int pid;
	int wait_errno;
	int status;
	struct tcb *tcp;
#ifdef LINUX
	struct rusage ru;
#ifdef __WALL
	static int wait4_options = __WALL;
#endif
#endif /* LINUX */

	while (nprocs != 0) {
		if (interrupted)
			return 0;
		if (interactive)
			sigprocmask(SIG_SETMASK, &empty_set, NULL);
#ifdef LINUX
#ifdef __WALL
		pid = wait4(-1, &status, wait4_options, cflag ? &ru : NULL);
		if (pid < 0 && (wait4_options & __WALL) && errno == EINVAL) {
			/* this kernel does not support __WALL */
			wait4_options &= ~__WALL;
			errno = 0;
			pid = wait4(-1, &status, wait4_options,
					cflag ? &ru : NULL);
		}
		if (pid < 0 && !(wait4_options & __WALL) && errno == ECHILD) {
			/* most likely a "cloned" process */
			pid = wait4(-1, &status, __WCLONE,
					cflag ? &ru : NULL);
			if (pid == -1) {
				fprintf(stderr, "strace: clone wait4 "
						"failed: %s\n", strerror(errno));
			}
		}
#else
		pid = wait4(-1, &status, 0, cflag ? &ru : NULL);
#endif /* __WALL */
#endif /* LINUX */
#ifdef SUNOS4
		pid = wait(&status);
#endif /* SUNOS4 */
		wait_errno = errno;
		if (interactive)
			sigprocmask(SIG_BLOCK, &blocked_set, NULL);

		if (pid == -1) {
			switch (wait_errno) {
			case EINTR:
				continue;
			case ECHILD:
				/*
				 * We would like to verify this case
				 * but sometimes a race in Solbourne's
				 * version of SunOS sometimes reports
				 * ECHILD before sending us SIGCHILD.
				 */
				return 0;
			default:
				errno = wait_errno;
				perror("strace: wait");
				return -1;
			}
		}
		if (pid == popen_pid) {
			if (WIFEXITED(status) || WIFSIGNALED(status))
				popen_pid = -1;
			continue;
		}
		if (debug)
			fprintf(stderr, " [wait(%#x) = %u]\n", status, pid);

		/* Look up `pid' in our table. */
		if ((tcp = pid2tcb(pid)) == NULL) {
#ifdef LINUX
			if (followfork) {
				/* This is needed to go with the CLONE_PTRACE
				   changes in process.c/util.c: we might see
				   the child's initial trap before we see the
				   parent return from the clone syscall.
				   Leave the child suspended until the parent
				   returns from its system call.  Only then
				   will we have the association of parent and
				   child so that we know how to do clearbpt
				   in the child.  */
				tcp = alloctcb(pid);
        // hmmm no longer seem to need it here - CDE_init_tcb_dir_fields(tcp); // pgbovine
				tcp->flags |= TCB_ATTACHED | TCB_SUSPENDED;
				if (!qflag)
					fprintf(stderr, "\
Process %d attached (waiting for parent)\n",
						pid);
			}
			else
				/* This can happen if a clone call used
				   CLONE_PTRACE itself.  */
#endif
			{
				fprintf(stderr, "unknown pid: %u\n", pid);
				if (WIFSTOPPED(status))
					ptrace(PTRACE_CONT, pid, (char *) 1, 0);
				exit(1);
			}
		}
		/* set current output file */
		outf = tcp->outf;
		curcol = tcp->curcol;
		if (cflag) {
#ifdef LINUX
			tv_sub(&tcp->dtime, &ru.ru_stime, &tcp->stime);
			tcp->stime = ru.ru_stime;
#endif /* !LINUX */
		}

		if (tcp->flags & TCB_SUSPENDED) {
			/*
			 * Apparently, doing any ptrace() call on a stopped
			 * process, provokes the kernel to report the process
			 * status again on a subsequent wait(), even if the
			 * process has not been actually restarted.
			 * Since we have inspected the arguments of suspended
			 * processes we end up here testing for this case.
			 */
			continue;
		}
		if (WIFSIGNALED(status)) {
			if (pid == strace_child)
				exit_code = 0x100 | WTERMSIG(status);
			if (cflag != CFLAG_ONLY_STATS
			    && (qual_flags[WTERMSIG(status)] & QUAL_SIGNAL)) {
				printleader(tcp);
				tprintf("+++ killed by %s %s+++",
					signame(WTERMSIG(status)),
#ifdef WCOREDUMP
					WCOREDUMP(status) ? "(core dumped) " :
#endif
					"");
				printtrailer();
			}
#ifdef TCB_GROUP_EXITING
			handle_group_exit(tcp, -1);
#else
			droptcb(tcp);
#endif
			continue;
		}
		if (WIFEXITED(status)) {
			if (pid == strace_child)
				exit_code = WEXITSTATUS(status);
			if (debug)
				fprintf(stderr, "pid %u exited with %d\n", pid, WEXITSTATUS(status));
			if ((tcp->flags & (TCB_ATTACHED|TCB_STARTUP)) == TCB_ATTACHED
#ifdef TCB_GROUP_EXITING
			    && !(tcp->parent && (tcp->parent->flags & TCB_GROUP_EXITING))
			    && !(tcp->flags & TCB_GROUP_EXITING)
#endif
			) {
				fprintf(stderr,
					"PANIC: attached pid %u exited with %d\n",
					pid, WEXITSTATUS(status));
			}
			if (tcp == tcp_last) {
				if ((tcp->flags & (TCB_INSYSCALL|TCB_REPRINT)) == TCB_INSYSCALL)
					tprintf(" <unfinished ... exit status %d>\n",
						WEXITSTATUS(status));
				tcp_last = NULL;
			}
#ifdef TCB_GROUP_EXITING
			handle_group_exit(tcp, -1);
#else
			droptcb(tcp);
#endif
			continue;
		}
		if (!WIFSTOPPED(status)) {
			fprintf(stderr, "PANIC: pid %u not stopped\n", pid);
			droptcb(tcp);
			continue;
		}
		if (debug)
			fprintf(stderr, "pid %u stopped, [%s]\n",
				pid, signame(WSTOPSIG(status)));

		/*
		 * Interestingly, the process may stop
		 * with STOPSIG equal to some other signal
		 * than SIGSTOP if we happend to attach
		 * just before the process takes a signal.
		 * A no-MMU vforked child won't send up a signal,
		 * so skip the first (lost) execve notification.
		 */
		if ((tcp->flags & TCB_STARTUP) &&
		    (WSTOPSIG(status) == SIGSTOP || strace_vforked)) {
			/*
			 * This flag is there to keep us in sync.
			 * Next time this process stops it should
			 * really be entering a system call.
			 */
			tcp->flags &= ~TCB_STARTUP;
			if (tcp->flags & TCB_BPTSET) {
				/*
				 * One example is a breakpoint inherited from
				 * parent through fork ().
				 */
				if (clearbpt(tcp) < 0) /* Pretty fatal */ {
					droptcb(tcp);
					cleanup();
					return -1;
				}
			}
			goto tracing;
		}

		if (WSTOPSIG(status) != SIGTRAP) {
			if (WSTOPSIG(status) == SIGSTOP &&
					(tcp->flags & TCB_SIGTRAPPED)) {
				/*
				 * Trapped attempt to block SIGTRAP
				 * Hope we are back in control now.
				 */
				tcp->flags &= ~(TCB_INSYSCALL | TCB_SIGTRAPPED);
				if (ptrace_restart(PTRACE_SYSCALL, tcp, 0) < 0) {
					cleanup();
					return -1;
				}
				continue;
			}
			if (cflag != CFLAG_ONLY_STATS
			    && (qual_flags[WSTOPSIG(status)] & QUAL_SIGNAL)) {
				unsigned long addr = 0;
				long pc = 0;
#if defined(PT_CR_IPSR) && defined(PT_CR_IIP) && defined(PT_GETSIGINFO)
#				define PSR_RI	41
				struct siginfo si;
				long psr;

				upeek(tcp, PT_CR_IPSR, &psr);
				upeek(tcp, PT_CR_IIP, &pc);

				pc += (psr >> PSR_RI) & 0x3;
				ptrace(PT_GETSIGINFO, pid, 0, (long) &si);
				addr = (unsigned long) si.si_addr;
#elif defined PTRACE_GETSIGINFO
				if (WSTOPSIG(status) == SIGSEGV ||
				    WSTOPSIG(status) == SIGBUS) {
					siginfo_t si;
					if (ptrace(PTRACE_GETSIGINFO, pid,
						   0, &si) == 0)
						addr = (unsigned long)
							si.si_addr;
				}
#endif
        // pgbovine - silence signal printouts
        /*
				printleader(tcp);
				tprintf("--- %s (%s) @ %lx (%lx) ---",
					signame(WSTOPSIG(status)),
					strsignal(WSTOPSIG(status)), pc, addr);
				printtrailer();
        */
			}
			if (((tcp->flags & TCB_ATTACHED) ||
			     tcp->nclone_threads > 0) &&
				!sigishandled(tcp, WSTOPSIG(status))) {
#ifdef TCB_GROUP_EXITING
				handle_group_exit(tcp, WSTOPSIG(status));
#else
				detach(tcp, WSTOPSIG(status));
#endif
				continue;
			}
			if (ptrace_restart(PTRACE_SYSCALL, tcp, WSTOPSIG(status)) < 0) {
				cleanup();
				return -1;
			}
			tcp->flags &= ~TCB_SUSPENDED;
			continue;
		}
		/* we handled the STATUS, we are permitted to interrupt now. */
		if (interrupted)
			return 0;
		if (trace_syscall(tcp) < 0 && !tcp->ptrace_errno) {
			/* ptrace() failed in trace_syscall() with ESRCH.
			 * Likely a result of process disappearing mid-flight.
			 * Observed case: exit_group() terminating
			 * all processes in thread group. In this case, threads
			 * "disappear" in an unpredictable moment without any
			 * notification to strace via wait().
			 */
			if (tcp->flags & TCB_ATTACHED) {
				if (tcp_last) {
					/* Do we have dangling line "syscall(param, param"?
					 * Finish the line then. We cannot
					 */
					tcp_last->flags |= TCB_REPRINT;
					tprintf(" <unfinished ...>");
					printtrailer();
				}
				detach(tcp, 0);
			} else {
				ptrace(PTRACE_KILL,
					tcp->pid, (char *) 1, SIGTERM);
				droptcb(tcp);
			}
			continue;
		}
		if (tcp->flags & TCB_EXITING) {
#ifdef TCB_GROUP_EXITING
			if (tcp->flags & TCB_GROUP_EXITING) {
				if (handle_group_exit(tcp, 0) < 0)
					return -1;
				continue;
			}
#endif
			if (tcp->flags & TCB_ATTACHED)
				detach(tcp, 0);
			else if (ptrace_restart(PTRACE_CONT, tcp, 0) < 0) {
				cleanup();
				return -1;
			}
			continue;
		}
		if (tcp->flags & TCB_SUSPENDED) {
			if (!qflag)
				fprintf(stderr, "Process %u suspended\n", pid);
			continue;
		}
	tracing:
		/* Remember current print column before continuing. */
		tcp->curcol = curcol;
		if (ptrace_restart(PTRACE_SYSCALL, tcp, 0) < 0) {
			cleanup();
			return -1;
		}
	}
	return 0;
}

#endif /* !USE_PROCFS */

#include <stdarg.h>

void
tprintf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	if (outf) {
		int n = vfprintf(outf, fmt, args);
		if (n < 0) {
			if (outf != stderr)
				perror(outfname == NULL
				       ? "<writing to pipe>" : outfname);
		} else
			curcol += n;
	}
	va_end(args);
	return;
}

void
printleader(tcp)
struct tcb *tcp;
{
	if (tcp_last) {
		if (tcp_last->ptrace_errno) {
			if (tcp_last->flags & TCB_INSYSCALL) {
				tprintf(" <unavailable>)");
				tabto(acolumn);
			}
			tprintf("= ? <unavailable>\n");
			tcp_last->ptrace_errno = 0;
		} else if (!outfname || followfork < 2 || tcp_last == tcp) {
			tcp_last->flags |= TCB_REPRINT;
			tprintf(" <unfinished ...>\n");
		}
	}
	curcol = 0;
	if ((followfork == 1 || pflag_seen > 1) && outfname)
		tprintf("%-5d ", tcp->pid);
	else if (nprocs > 1 && !outfname)
		tprintf("[pid %5u] ", tcp->pid);
	if (tflag) {
		char str[sizeof("HH:MM:SS")];
		struct timeval tv, dtv;
		static struct timeval otv;

		gettimeofday(&tv, NULL);
		if (rflag) {
			if (otv.tv_sec == 0)
				otv = tv;
			tv_sub(&dtv, &tv, &otv);
			tprintf("%6ld.%06ld ",
				(long) dtv.tv_sec, (long) dtv.tv_usec);
			otv = tv;
		}
		else if (tflag > 2) {
			tprintf("%ld.%06ld ",
				(long) tv.tv_sec, (long) tv.tv_usec);
		}
		else {
			time_t local = tv.tv_sec;
			strftime(str, sizeof(str), "%T", localtime(&local));
			if (tflag > 1)
				tprintf("%s.%06ld ", str, (long) tv.tv_usec);
			else
				tprintf("%s ", str);
		}
	}
	if (iflag)
		printcall(tcp);
}

void
tabto(col)
int col;
{
	if (curcol < col)
		tprintf("%*s", col - curcol, "");
}

void
printtrailer(void)
{
  // pgbovine - don't print anything!
	//tprintf("\n");
	tcp_last = NULL;
}

#ifdef HAVE_MP_PROCFS

int
mp_ioctl(int fd, int cmd, void *arg, int size)
{
	struct iovec iov[2];
	int n = 1;

	iov[0].iov_base = &cmd;
	iov[0].iov_len = sizeof cmd;
	if (arg) {
		++n;
		iov[1].iov_base = arg;
		iov[1].iov_len = size;
	}

	return writev(fd, iov, n);
}

#endif
