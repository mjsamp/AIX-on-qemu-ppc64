/* Copyright 1988,1990,1993,1994 by Paul Vixie
 * All rights reserved
 *
 * Distribute freely, except: don't remove my name from the source or
 * documentation (don't take credit for my work), mark your changes (don't
 * get me blamed for your possible bugs), don't alter or remove this
 * notice.  May be sold if buildable source is provided to buyer.  No
 * warrantee of any kind, express or implied, is included with this
 * software; use at your own risk, responsibility for damages (if any) to
 * anyone resulting from the use of this software rests entirely with the
 * user.
 *
 * Send bug reports, bug fixes, enhancements, requests, flames, etc., and
 * I'll try to keep a version up to date.  I can be reached as follows:
 * Paul Vixie          <paul@vix.com>          uunet!decwrl!vixie!paul
 */

#include <signal.h>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(sequent)
# include <sys/universe.h>
#endif
#if defined(SYSLOG)
# include <syslog.h>
#endif
#if defined(USE_PAM)
#include <security/pam_appl.h>
static pam_handle_t *pamh = NULL;
static const struct pam_conv conv = {
	NULL
};
#define PAM_FAIL_CHECK if (retcode != PAM_SUCCESS) { \
	fprintf(stderr,"\n%s\n",pam_strerror(pamh, retcode)); \
	syslog(LOG_ERR,"%s",pam_strerror(pamh, retcode)); \
	pam_end(pamh, retcode); exit(1); \
   }
#endif

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
/* #include <selinux/get_context_list.h> */
#endif


static void		child_process __P((entry *, user *)),
			do_univ __P((user *));

/* Build up the job environment from the PAM environment plus the
   crontab environment */
static char ** build_env(char **cronenv)
{
        char **jobenv = cronenv;
#if defined(USE_PAM)
        char **pamenv = pam_getenvlist(pamh);
        char *cronvar;
        int count = 0;

        jobenv = env_copy(pamenv);

        /* Now add the cron environment variables. Since env_set()
           overwrites existing variables, this will let cron's
           environment settings override pam's */

        while ((cronvar = cronenv[count++])) {
                if (!(jobenv = env_set(jobenv, cronvar))) {
                        syslog(LOG_ERR, "Setting Cron environment variable %s failed", cronvar);
                        return NULL;
                }
        }
#endif
    return jobenv;
}

void
do_command(e, u)
	entry	*e;
	user	*u;
{
	Debug(DPROC, ("[%d] do_command(%s, (%s,%d,%d))\n",
		getpid(), e->cmd, u->name, e->uid, e->gid))

	/* fork to become asynchronous -- parent process is done immediately,
	 * and continues to run the normal cron code, which means return to
	 * tick().  the child and grandchild don't leave this function, alive.
	 *
	 * vfork() is unsuitable, since we have much to do, and the parent
	 * needs to be able to run off and fork other processes.
	 */
	switch (fork()) {
	case -1:
		log_it("CRON",getpid(),"error","can't fork");
		break;
	case 0:
		/* child process */
		acquire_daemonlock(1);
		child_process(e, u);
		Debug(DPROC, ("[%d] child process done, exiting\n", getpid()))
		_exit(OK_EXIT);
		break;
	default:
		/* parent process */
		break;
	}
	Debug(DPROC, ("[%d] main process returning to work\n", getpid()))
}


/*
 * CROND
 *  - cron (runs child_process);
 *    - cron (runs exec sh -c 'tab entry');
 *    - cron (writes any %-style stdin to the command);
 *    - mail (popen writes any stdout to mailcmd);
 */

static void
child_process(e, u)
	entry	*e;
	user	*u;
{
	int		stdin_pipe[2];
	FILE		*tmpout;
	register char	*input_data;
	char		*usernm, *mailto;
	int		children = 0;
	pid_t		job_pid;

#if defined(USE_PAM)
	int		retcode = 0;
#endif

	Debug(DPROC, ("[%d] child_process('%s')\n", getpid(), e->cmd))

	/* mark ourselves as different to PS command watchers by upshifting
	 * our program name.  This has no effect on some kernels.
	 */
	/*local*/{
		register char	*pch;

		for (pch = ProgramName;  *pch;  pch++)
			*pch = MkUpper(*pch);
	}

	/* discover some useful and important environment settings
	 */
	usernm = env_get("LOGNAME", e->envp);
	mailto = env_get("MAILTO", e->envp);

	/* Check for arguments */
	if (mailto) {
		const char	*end;

		/* These chars have to match those cron_popen()
		 * uses to split the command string */
		mailto += strspn(mailto, " \t\n");
		end = mailto + strcspn(mailto, " \t\n");
		if (*mailto == '-' || *end != '\0') {
			printf("Bad Mailto karma.\n");
			log_it("CRON",getpid(),"error","bad mailto");
			mailto = NULL;
		}
	}

#ifdef USE_SIGCHLD
	/* our parent is watching for our death by catching SIGCHLD.  we
	 * do not care to watch for our children's deaths this way -- we
	 * use wait() explictly.  so we have to disable the signal (which
	 * was inherited from the parent).
	 */
#ifdef DEBIAN
	(void) signal(SIGCHLD, SIG_DFL);
#else
	(void) signal(SIGCHLD, SIG_IGN);
#endif
#else
	/* on system-V systems, we are ignoring SIGCLD.  we have to stop
	 * ignoring it now or the wait() in cron_pclose() won't work.
	 * because of this, we have to wait() for our children here, as well.
	 */
	(void) signal(SIGCLD, SIG_DFL);
#endif /*BSD*/

	/* create a pipe to talk to our future child
	 */
	pipe(stdin_pipe);	/* child's stdin */
	/* child's stdout */
	if ((tmpout = tmpfile()) == NULL) {
		log_it("CRON", getpid(), "error", "create tmpfile");
		exit(ERROR_EXIT);
	}
	
	/* since we are a forked process, we can diddle the command string
	 * we were passed -- nobody else is going to use it again, right?
	 *
	 * if a % is present in the command, previous characters are the
	 * command, and subsequent characters are the additional input to
	 * the command.  Subsequent %'s will be transformed into newlines,
	 * but that happens later.
	 *
	 * If there are escaped %'s, remove the escape character.
	 */
	/*local*/{
		register int escaped = FALSE;
		register int ch;
		register char *p;

		for (input_data = p = e->cmd; (ch = *input_data);
		    input_data++, p++) {
			if (p != input_data)
				*p = ch;
			if (escaped) {
				if (ch == '%' || ch == '\\')
					*--p = ch;
				escaped = FALSE;
				continue;
			}
			if (ch == '\\') {
				escaped = TRUE;
				continue;
			}
			if (ch == '%') {
				*input_data++ = '\0';
				break;
			}
		}
		*p = '\0';
	}

#if defined(USE_PAM)
	retcode = pam_start("cron", usernm, &conv, &pamh);
	PAM_FAIL_CHECK;
	retcode = pam_set_item(pamh, PAM_TTY, "cron");
	PAM_FAIL_CHECK;
	retcode = pam_acct_mgmt(pamh, PAM_SILENT);
	PAM_FAIL_CHECK;
	retcode = pam_setcred(pamh, PAM_ESTABLISH_CRED | PAM_SILENT);
	PAM_FAIL_CHECK;
	retcode = pam_open_session(pamh, PAM_SILENT);
	PAM_FAIL_CHECK;

#endif

	/* fork again, this time so we can exec the user's command.
	 */
	switch (job_pid = fork()) {
	case -1:
		log_it("CRON",getpid(),"error","can't fork");
		exit(ERROR_EXIT);
		/*NOTREACHED*/
	case 0:
		Debug(DPROC, ("[%d] grandchild process fork()'ed\n",
			      getpid()))

		/* write a log message .  we've waited this long to do it
		 * because it was not until now that we knew the PID that
		 * the actual user command shell was going to get and the
		 * PID is part of the log message.
		 */
		if ( (log_level & CRON_LOG_JOBSTART) && ! (log_level & CRON_LOG_JOBPID)) {
			char *x = mkprints((u_char *)e->cmd, strlen(e->cmd));
			log_it(usernm, getpid(), "CMD", x);
			free(x);
		}
		/* nothing to log from now on. close the log files.
		 */
		log_close();

		/* get new pgrp, void tty, etc.
		 */
		(void) setsid();

		/* close the pipe ends that we won't use.  this doesn't affect
		 * the parent, who has to read and write them; it keeps the
		 * kernel from recording us as a potential client TWICE --
		 * which would keep it from sending SIGPIPE in otherwise
		 * appropriate circumstances.
		 */
		close(stdin_pipe[WRITE_PIPE]);

		/* grandchild process.  make std{in,out} be the ends of
		 * pipes opened by our daddy; make stderr go to stdout.
		 */
		/* Closes are unnecessary -- let dup2() do it */

		  /* close(STDIN) */; dup2(stdin_pipe[READ_PIPE], STDIN);
		  dup2(fileno(tmpout), STDOUT);
		  /* close(STDERR)*/; dup2(STDOUT, STDERR);


		/* close the pipe we just dup'ed.  The resources will remain.
		 */
		close(stdin_pipe[READ_PIPE]);
		fclose(tmpout);

		/* set our login universe.  Do this in the grandchild
		 * so that the child can invoke /usr/lib/sendmail
		 * without surprises.
		 */
		do_univ(u);

		/* set our directory, uid and gid.  Set gid first, since once
		 * we set uid, we've lost root privledges.
		 */
		if (setgid(e->gid) !=0) {
		  char msg[256];
		  snprintf(msg, 256, "do_command:setgid(%lu) failed: %s",
			   (unsigned long) e->gid, strerror(errno));
		  log_it("CRON",getpid(),"error",msg);
		  exit(ERROR_EXIT);
		}
# if defined(BSD) || defined(POSIX)
		if (initgroups(env_get("LOGNAME", e->envp), e->gid) !=0) {
		  char msg[256];
		  snprintf(msg, 256, "do_command:initgroups(%lu) failed: %s",
			   (unsigned long) e->gid, strerror(errno));
		  log_it("CRON",getpid(),"error",msg);
		  exit(ERROR_EXIT);
		}
# endif
		if (setuid(e->uid) !=0) { /* we aren't root after this... */
		  char msg[256];
		  snprintf(msg, 256, "do_command:setuid(%lu) failed: %s",
			   (unsigned long) e->uid, strerror(errno)); 
		  log_it("CRON",getpid(),"error",msg);
		  exit(ERROR_EXIT);
		}	
		chdir(env_get("HOME", e->envp));

		/* exec the command.
		 */
		{
                        char    **jobenv = build_env(e->envp); 
                        char	*shell = env_get("SHELL", jobenv);
# if DEBUGGING
			if (DebugFlags & DTEST) {
				fprintf(stderr,
				"debug DTEST is on, not exec'ing command.\n");
				fprintf(stderr,
				"\tcmd='%s' shell='%s'\n", e->cmd, shell);
				_exit(OK_EXIT);
			}
# endif /*DEBUGGING*/
#if 0
			{
			  struct sigaction oact;
			  sigaction(SIGCHLD, NULL, &oact);
			}
			fprintf(stdout,"error");
#endif
#ifdef WITH_SELINUX
			if (is_selinux_enabled() > 0) {
			    if (u->scontext != 0L) {
                                if (setexeccon(u->scontext) < 0) {
                                    if (security_getenforce() > 0) {
                                        fprintf(stderr, "Could not set exec context to %s for user  %s\n", u->scontext,u->name);
                                        _exit(ERROR_EXIT);
                                    }
			        }
                            }
			    else if(security_getenforce() > 0)
			    {
                                fprintf(stderr, "Error, must have a security context for the cron job when in enforcing mode.\nUser %s.\n", u->name);
                                _exit(ERROR_EXIT);
			    }
			}
#endif
                        execle(shell, shell, "-c", e->cmd, (char *)0, jobenv);
			fprintf(stderr, "%s: execle: %s\n", shell, strerror(errno));
			_exit(ERROR_EXIT);
		}
		break;
	default:
		/* parent process */
		/* write a log message if we want the parent and child
		 * PID values
		 */
		if ( (log_level & CRON_LOG_JOBSTART) && (log_level & CRON_LOG_JOBPID)) {
			char logcmd[MAX_COMMAND + 8];
			snprintf(logcmd, sizeof(logcmd), "[%d] %s", (int) job_pid, e->cmd);
			char *x = mkprints((u_char *)logcmd, strlen(logcmd));
			log_it(usernm, getpid(), "CMD", x);
			free(x);
		}
		break;
	}

	children++;

	/* middle process, child of original cron, parent of process running
	 * the user's command.
	 */

	Debug(DPROC, ("[%d] child continues, closing pipes\n", getpid()))

	/* close the end of the pipe that will only be referenced in the
	 * grandchild process...
	 */
	close(stdin_pipe[READ_PIPE]);

	/*
	 * write, to the pipe connected to child's stdin, any input specified
	 * after a % in the crontab entry.  while we copy, convert any
	 * additional %'s to newlines.  when done, if some characters were
	 * written and the last one wasn't a newline, write a newline.
	 *
	 * Note that if the input data won't fit into one pipe buffer (2K
	 * or 4K on most BSD systems), and the child doesn't read its stdin,
	 * we would block here.  thus we must fork again.
	 */

	if (*input_data && fork() == 0) {
		register FILE	*out = fdopen(stdin_pipe[WRITE_PIPE], "w");
		register int	need_newline = FALSE;
		register int	escaped = FALSE;
		register int	ch;

		Debug(DPROC, ("[%d] child2 sending data to grandchild\n", getpid()))

		/* translation:
		 *	\% -> %
		 *	%  -> \n
		 *	\x -> \x	for all x != %
		 */
		while ((ch = *input_data++) != '\0') {
			if (escaped) {
				if (ch != '%')
					putc('\\', out);
			} else {
				if (ch == '%')
					ch = '\n';
			}

			if (!(escaped = (ch == '\\'))) {
				putc(ch, out);
				need_newline = (ch != '\n');
			}
		}
		if (escaped)
			putc('\\', out);
		if (need_newline)
			putc('\n', out);

		/* close the pipe, causing an EOF condition.  fclose causes
		 * stdin_pipe[WRITE_PIPE] to be closed, too.
		 */
		fclose(out);

		Debug(DPROC, ("[%d] child2 done sending to grandchild\n", getpid()))
		exit(0);
	}

	/* close the pipe to the grandkiddie's stdin, since its wicked uncle
	 * ernie back there has it open and will close it when he's done.
	 */
	close(stdin_pipe[WRITE_PIPE]);

	children++;

	/*
	 * read output from the grandchild.  it's stderr has been redirected to
	 * it's stdout, which has been redirected to our pipe.  if there is any
	 * output, we'll be mailing it to the user whose crontab this is...
	 * when the grandchild exits, we'll get EOF.
	 */

	/* wait for children to die.
	 */
	int status = 0;
	for (;  children > 0;  children--)
	{
		char		msg[256];
		WAIT_T		waiter;
		PID_T		pid;

		Debug(DPROC, ("[%d] waiting for grandchild #%d to finish\n",
			getpid(), children))
		pid = wait(&waiter);
		if (pid < OK) {
			Debug(DPROC, ("[%d] no more grandchildren\n", getpid()))
			break;
		}
		Debug(DPROC, ("[%d] grandchild #%d finished, status=%04x\n",
			getpid(), pid, WEXITSTATUS(waiter)))

		if (log_level & CRON_LOG_JOBFAILED) {
			if (WIFEXITED(waiter) && WEXITSTATUS(waiter)) {
				status = waiter;
				snprintf(msg, 256, "grandchild #%d failed with exit "
					"status %d", pid, WEXITSTATUS(waiter));
				log_it("CRON", getpid(), "error", msg);
			} else if (WIFSIGNALED(waiter)) {
				status = waiter;
				snprintf(msg, 256, "grandchild #%d terminated by signal"
					" %d%s", pid, WTERMSIG(waiter),
					WCOREDUMP(waiter) ? ", dumped core" : "");
				log_it("CRON", getpid(), "error", msg);
			} 
		}
	}

/** Finally, send any output of the command to the mailer; also, alert
 the user if their job failed.  Avoid popening the mailcmd until now
 since sendmail may time out, and to write info about the exit
 status.**/
	
	long pos;
	struct stat	mcsb;
	int		statret;	

	fseek(tmpout, 0, SEEK_END);
	pos = ftell(tmpout);
	fseek(tmpout, 0, SEEK_SET);

	Debug(DPROC|DEXT, ("[%d] got %ld bytes data from grandchild tmpfile\n",
				getpid(), (long) pos))
	if (pos == 0)
		goto mail_finished;

	/** get name of recipient.**/
	if (mailto == NULL)
		mailto = usernm;
	else if (!*mailto)
                goto mail_finished;

	/* Don't send mail if MAILCMD is not available */
	if ((statret = stat(MAILCMD, &mcsb)) != 0) {
		Debug(DPROC|DEXT, ("%s not found, not sending mail\n", MAILCMD))
		if (pos > 0) {
			log_it("CRON", getpid(), "info", "No MTA installed, discarding output");
		}
		goto mail_finished;
	} else {
		Debug(DPROC|DEXT, ("%s found, will send mail\n", MAILCMD))
	}

	register FILE	*mail = NULL;
	register int	bytes = 0;

	register char	**env;
	char    	**jobenv = build_env(e->envp); 
	auto char	mailcmd[MAX_COMMAND];
	auto char	hostname[MAXHOSTNAMELEN];
	char    	*content_type = env_get("CONTENT_TYPE",jobenv),
			*content_transfer_encoding = env_get("CONTENT_TRANSFER_ENCODING",jobenv);

	(void) gethostname(hostname, MAXHOSTNAMELEN);
	(void) snprintf(mailcmd, sizeof(mailcmd),
			MAILARGS, MAILCMD, mailto);
	if (!(mail = cron_popen(mailcmd, "w", e))) {
		perror(MAILCMD);
		(void) _exit(ERROR_EXIT);
	}
	fprintf(mail, "From: root (Cron Daemon)\n");
	fprintf(mail, "To: %s\n", mailto);
	fprintf(mail, "Subject: Cron <%s@%s> %s%s\n",
			usernm,
			fqdn_in_subject ? hostname : first_word(hostname, "."),
			e->cmd, status?" (failed)":"");
# if defined(MAIL_DATE)
	fprintf(mail, "Date: %s\n",
			arpadate(&StartTime));
# endif /* MAIL_DATE */
	fprintf(mail, "MIME-Version: 1.0\n");
	if ( content_type == 0L ) {
		fprintf(mail, "Content-Type: text/plain; charset=%s\n",
				cron_default_mail_charset
		       );
	} else {   
		/* user specified Content-Type header.
		 * disallow new-lines for security reasons
		 * (else users could specify arbitrary mail headers!)
		 */
		char *nl=content_type;
		size_t ctlen = strlen(content_type);

		while(  (*nl != '\0')
				&& ((nl=strchr(nl,'\n')) != 0L)
				&& (nl < (content_type+ctlen))
		     ) *nl = ' ';
		fprintf(mail,"Content-Type: %s\n", content_type);
	}
	if ( content_transfer_encoding != 0L ) {
		char *nl=content_transfer_encoding;
		size_t ctlen = strlen(content_transfer_encoding);
		while(  (*nl != '\0')
				&& ((nl=strchr(nl,'\n')) != 0L)
				&& (nl < (content_transfer_encoding+ctlen))
		     ) *nl = ' ';

		fprintf(mail,"Content-Transfer-Encoding: %s\n", content_transfer_encoding);
	}
	else
		fprintf(mail, "Content-Transfer-Encoding: 8bit\n");

	for (env = e->envp;  *env;  env++)
		fprintf(mail, "X-Cron-Env: <%s>\n",
				*env);
	fputc('\n', mail);

/** Append the actual output of the child to the mail**/
	
	char buf[4096];
	int ret, remain;

	while(1) {
		if ((ret = fread(buf, 1, sizeof(buf), tmpout)) == 0)
			break;
		for (remain = ret; remain != 0; ) {
			ret = fwrite(buf, 1, remain, mail);
			if (ret > 0) {
				remain -= ret;
				bytes += ret;
				continue;
			}
			/** XXX error**/
			break;
		}
	}

	Debug(DPROC, ("[%d] closing pipe to mail\n", getpid()))
	status = cron_pclose(mail);

	/* if there was output and we could not mail it,
	 * log the facts so the poor user can figure out
	 * what's going on.
	 */
	if (status) {
		char buf[MAX_TEMPSTR];
		snprintf(buf, MAX_TEMPSTR,
				"mailed %d byte%s of output "
				"but got status 0x%04x from MTA\n",
				bytes, (bytes==1)?"":"s", status);
		log_it(usernm, getpid(), "MAIL", buf);
	}

	if (ferror(tmpout)) {
		log_it(usernm, getpid(), "MAIL", "stream error reading output");
	}

mail_finished:
	fclose(tmpout);

	if (log_level & CRON_LOG_JOBEND) {
		char *x;
		if (log_level & CRON_LOG_JOBPID) {
			char logcmd[MAX_COMMAND + 8];
			snprintf(logcmd, sizeof(logcmd), "[%d] %s", (int) job_pid, e->cmd);
			x = mkprints((u_char *)logcmd, strlen(logcmd));
		} else {
			x = mkprints((u_char *)e->cmd, strlen(e->cmd));
		}
		log_it(usernm, job_pid, "END", x);
		free(x);
	}

#if defined(USE_PAM)
	pam_setcred(pamh, PAM_DELETE_CRED | PAM_SILENT);
	retcode = pam_close_session(pamh, PAM_SILENT);
	pam_end(pamh, retcode);
#endif
}


static void
do_univ(u)
	user	*u;
{
#if defined(sequent)
/* Dynix (Sequent) hack to put the user associated with
 * the passed user structure into the ATT universe if
 * necessary.  We have to dig the gecos info out of
 * the user's password entry to see if the magic
 * "universe(att)" string is present.
 */

	struct	passwd	*p;
	char	*s;
	int	i;

	p = getpwuid(u->uid);
	(void) endpwent();

	if (p == NULL)
		return;

	s = p->pw_gecos;

	for (i = 0; i < 4; i++)
	{
		if ((s = strchr(s, ',')) == NULL)
			return;
		s++;
	}
	if (strcmp(s, "universe(att)"))
		return;

	(void) universe(U_ATT);
#endif
}
