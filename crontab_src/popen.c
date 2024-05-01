/*
 * Copyright (c) 1988 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software written by Ken Arnold and
 * published in UNIX Review, Vol. 6, No. 8.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

/* this came out of the ftpd sources; it's been modified to avoid the
 * globbing stuff since we don't need it.  also execvp instead of execv.
 */

#ifndef lint
static char sccsid[] = "@(#)popen.c	5.7 (Berkeley) 2/14/89";
#endif /* not lint */

#include <signal.h>

#if defined(BSD) || defined(POSIX)
#  include <grp.h>
#endif


#define MAX_ARGS 100
#define WANT_GLOBBING 0

/*
 * Special version of popen which avoids call to shell.  This insures noone
 * may create a pipe to a hidden program as a side effect of a list or dir
 * command.
 */
static PID_T *pids;
static int fds;

FILE *
cron_popen(program, type, e)
	char *program, *type;
	entry *e;
{
	register char *cp;
	FILE *iop;
	int argc, pdes[2];
	PID_T pid;
	char *argv[MAX_ARGS + 1];
#if WANT_GLOBBING
	char **pop, *vv[2];
	int gargc;
	char *gargv[1000];
	extern char **glob(), **copyblk();
#endif

	if ((*type != 'r' && *type != 'w') || type[1])
		return(NULL);

	if (!pids) {
		if ((fds = getdtablesize()) <= 0)
			return(NULL);
		if (!(pids = (PID_T *)malloc((u_int)(fds * sizeof(PID_T)))))
			return(NULL);
		/**bzero((char *)pids, fds * sizeof(PID_T));**/
	}
	if (pipe(pdes) < 0)
		return(NULL);

	/* break up string into pieces */
	for (argc = 0, cp = program; argc < MAX_ARGS; cp = NULL)
		if (!(argv[argc++] = strtok(cp, " \t\n")))
			break;
    argv[MAX_ARGS] = NULL;

#if WANT_GLOBBING
	/* glob each piece */
	gargv[0] = argv[0];
	for (gargc = argc = 1; argv[argc]; argc++) {
		if (!(pop = glob(argv[argc]))) {	/* globbing failed */
			vv[0] = argv[argc];
			vv[1] = NULL;
			pop = copyblk(vv);
		}
		argv[argc] = (char *)pop;		/* save to free later */
		while (*pop && gargc < 1000)
			gargv[gargc++] = *pop++;
	}
	gargv[gargc] = NULL;
#endif

	iop = NULL;
	switch(pid = fork()) {
	case -1:			/* error */
		(void)close(pdes[0]);
		(void)close(pdes[1]);
		goto pfree;
		/* NOTREACHED */
	case 0:				/* child */
		if (*type == 'r') {
			if (pdes[1] != 1) {
				dup2(pdes[1], 1);
				dup2(pdes[1], 2);	/* stderr, too! */
				(void)close(pdes[1]);
			}
			(void)close(pdes[0]);
		} else {
			if (pdes[0] != 0) {
				dup2(pdes[0], 0);
				(void)close(pdes[0]);
			}
			(void)close(pdes[1]);
		}
 		/* set our directory, uid and gid.  Set gid first, since once
         * we set uid, we've lost root privleges.
         */
        if (setgid(e->gid) !=0) {
          char msg[256];
          snprintf(msg, 256, "popen:setgid(%lu) failed: %s",
               (unsigned long) e->gid, strerror(errno));
          log_it("CRON",getpid(),"error",msg);
          exit(ERROR_EXIT);
        }
# if defined(BSD) || defined(POSIX)
		if (initgroups(env_get("LOGNAME", e->envp), e->gid) !=0) {
		  char msg[256];
		  snprintf(msg, 256, "popen:initgroups(%lu) failed: %s",
			   (unsigned long) e->gid, strerror(errno));
		  log_it("CRON",getpid(),"error",msg);
		  exit(ERROR_EXIT);
                }
# endif
		if (setuid(e->uid) !=0) {
		  char msg[256];
		  snprintf(msg, 256, "popen: setuid(%lu) failed: %s",
			   (unsigned long) e->uid, strerror(errno)); 
		  log_it("CRON",getpid(),"error",msg);
		  exit(ERROR_EXIT);
		}	
		chdir(env_get("HOME", e->envp));

#if WANT_GLOBBING
		execvp(gargv[0], gargv);
#else
		execvp(argv[0], argv);
#endif
		_exit(1);
	}
	/* parent; assume fdopen can't fail...  */
	if (*type == 'r') {
		iop = fdopen(pdes[0], type);
		(void)close(pdes[1]);
	} else {
		iop = fdopen(pdes[1], type);
		(void)close(pdes[0]);
	}
	pids[fileno(iop)] = pid;

pfree:
#if WANT_GLOBBING
	for (argc = 1; argv[argc] != NULL; argc++) {
/*		blkfree((char **)argv[argc]);	*/
		free((char *)argv[argc]);
	}
#endif
	return(iop);
}

int
cron_pclose(iop)
	FILE *iop;
{
	register int fdes;
	sigset_t omask, mask;
	WAIT_T stat_loc;
	PID_T pid;

	/*
	 * pclose returns -1 if stream is not associated with a
	 * `popened' command, or, if already `pclosed'.
	 */
	if (pids == 0 || pids[fdes = fileno(iop)] == 0)
		return(-1);
	(void)fclose(iop);
	sigemptyset(&mask);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, &omask);
	pid = waitpid(pids[fdes], &stat_loc, 0);
	sigprocmask(SIG_SETMASK, &omask, NULL);
	pids[fdes] = 0;
	if (pid == -1 || !WIFEXITED(stat_loc))
		return -1;
	return WEXITSTATUS(stat_loc);
}
