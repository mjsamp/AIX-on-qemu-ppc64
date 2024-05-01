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

#if !defined(lint) && !defined(LINT)
static char rcsid[] = "$Id: cron.c,v 2.11 1994/01/15 20:43:43 vixie Exp $";
#endif


#define	MAIN_PROGRAM


#include "cron.h"
#include "job.c"
#include "do_command.c"
#include "misc.c"
#include "popen.c"
#include "database.c"
#include "env.c"
#include "user.c"
#include "entry.c"
#include "compat.c"
#include <signal.h>

#include <sys/types.h>
#include <fcntl.h>
#include <libgen.h>
#include <strings.h>

static	void	usage __P((void)),
		run_reboot_jobs __P((cron_db *)),
		find_jobs __P((time_min, cron_db *, int, int)),
		set_time __P((int)),
		cron_sleep __P((time_min)),
#ifdef USE_SIGCHLD
		sigchld_handler __P((int)),
#endif
		sighup_handler __P((int)),
		parse_args __P((int c, char *v[]));



static void
usage() {
#if DEBUGGING
	char **dflags;

	fprintf(stderr, "usage:  %s [-x [", ProgramName);
	for(dflags = DebugFlagNames; *dflags; dflags++)
		fprintf(stderr, "%s%s", *dflags, dflags[1] ? "," : "]");
	fprintf(stderr, "]\n");
#else
	fprintf(stderr, "usage: %s\n", ProgramName);
#endif
	exit(ERROR_EXIT);
}



main(argc, argv)
	int	argc;
	char	*argv[];
{
	cron_db	database;
	char *cs;

	ProgramName = basename(argv[0]);

#if defined(BSD)
	setlinebuf(stdout);
	setlinebuf(stderr);
#endif

	parse_args(argc, argv);

#ifdef USE_SIGCHLD
	(void) signal(SIGCHLD, sigchld_handler);
#else
	(void) signal(SIGCLD, SIG_IGN);
#endif
	(void) signal(SIGHUP, sighup_handler);

        /* Reopen stdin in case some idiot closed it before starting
           us - it will only be closed, but not having it open here
           screws up other things that will be opened */
        if (fdopen(0,"r") == NULL) {
            (void) open("dev/null", 0);
        }

	acquire_daemonlock(0);
	set_cron_uid();
	set_cron_cwd();

#if defined(POSIX)
	setenv("PATH", _PATH_DEFPATH, 1);
#endif

       /* Get the default locale character set for the mail
        * "Content-Type: ...; charset=" header
        */
       setlocale(LC_ALL,""); /* set locale to system defaults or to
                                that specified by any  LC_* env vars */
       setlocale(LC_COLLATE, "C"); /* Except for collation, since load_database() uses a-z */
       /* Except that "US-ASCII" is preferred to "ANSI_x3.4-1968" in MIME,
        * even though "ANSI_x3.4-1968" is the official charset name. */
       if ( ( cs = nl_langinfo( CODESET ) ) != 0L && 
               strcasecmp(cs, "ANSI_x3.4-1968") != 0 )
           strncpy( cron_default_mail_charset, cs, MAX_ENVSTR );
       else
           strcpy( cron_default_mail_charset, "US-ASCII" );

	/* if there are no debug flags turned on, fork as a daemon should.
	 */
# if DEBUGGING
	if (DebugFlags) {
# else
	if (0) {
# endif
		(void) fprintf(stderr, "[%d] cron started\n", getpid());
	} else if (!stay_foreground) {
		switch (fork()) {
		case -1:
			log_it("CRON",getpid(),"DEATH","can't fork");
			exit(0);
			break;
		case 0:
			/* child process */
			log_it("CRON",getpid(),"STARTUP","fork ok");
			(void) setsid();
			freopen("/dev/null", "r", stdin);
			freopen("/dev/null", "w", stdout);
			freopen("/dev/null", "w", stderr);
			break;
		default:
			/* parent process should just die */
			_exit(0);
		}
	}

	acquire_daemonlock(0);
	database.head = NULL;
	database.tail = NULL;
	database.sys_mtime = (time_t) 0;
	database.user_mtime = (time_t) 0;
#ifdef DEBIAN
	database.sysd_mtime = (time_t) 0;
#endif
	load_database(&database);

	set_time(TRUE);
	run_reboot_jobs(&database);
	timeRunning = virtualTime = clockTime;

	/*
	 * too many clocks, not enough time (Al. Einstein)
	 * These clocks are in minutes since the epoch (time()/60).
	 * virtualTime: is the time it *would* be if we woke up
	 * promptly and nobody ever changed the clock. It is
	 * monotonically increasing... unless a timejump happens.
	 * At the top of the loop, all jobs for 'virtualTime' have run.
	 * timeRunning: is the time we last awakened.
	 * clockTime: is the time when set_time was last called.
	 */
	while (TRUE) {
		time_min timeDiff;
		int wakeupKind;
		

		/* ... wait for the time (in minutes) to change ... */
		do {
			cron_sleep(timeRunning + 1);
			set_time(FALSE);
		} while (clockTime == timeRunning);
		timeRunning = clockTime;

		check_orphans(&database);
		load_database(&database);

		/*
		 * ... calculate how the current time differs from
		 * our virtual clock. Classify the change into one
		 * of 4 cases
		 */
		timeDiff = timeRunning - virtualTime;


		Debug(DSCH, ("[%d] pulse: %d = %d - %d\n",
            	    getpid(), timeDiff, timeRunning, virtualTime));

		/* shortcut for the most common case */
		if (timeDiff == 1) {
			virtualTime = timeRunning;
			find_jobs(virtualTime, &database, TRUE, TRUE);
		} else {
			wakeupKind = -1;
			if (timeDiff > -(3*MINUTE_COUNT))
				wakeupKind = 0;
			if (timeDiff > 0)
				wakeupKind = 1;
			if (timeDiff > 5)
				wakeupKind = 2;
			if (timeDiff > (3*MINUTE_COUNT))
				wakeupKind = 3;

			switch (wakeupKind) {
			case 1:
				/*
				 * case 1: timeDiff is a small positive number
				 * (wokeup late) run jobs for each virtual minute
				 * until caught up.
				 */

				Debug(DSCH, ("[%d], normal case %d minutes to go\n",
				    getpid(), timeRunning - virtualTime))
				do {
					if (job_runqueue())
						sleep(10);
					virtualTime++;
					find_jobs(virtualTime, &database, TRUE, TRUE);
				} while (virtualTime< timeRunning);
				break;

			case 2:
				/*
				 * case 2: timeDiff is a medium-sized positive number,
				 * for example because we went to DST run wildcard
				 * jobs once, then run any fixed-time jobs that would
				 * otherwise be skipped if we use up our minute
				 * (possible, if there are a lot of jobs to run) go
				 * around the loop again so that wildcard jobs have
				 * a chance to run, and we do our housekeeping
				 */

				Debug(DSCH, ("[%d], DST begins %d minutes to go\n",
				    getpid(), timeRunning - virtualTime))
				/* run wildcard jobs for current minute */
				find_jobs(timeRunning, &database, TRUE, FALSE);
	
				/* run fixed-time jobs for each minute missed */ 
				do {
					if (job_runqueue())
						sleep(10);
					virtualTime++;
					find_jobs(virtualTime, &database, FALSE, TRUE);
					set_time(FALSE);
				} while (virtualTime< timeRunning &&
				    clockTime == timeRunning);
				break;
	
			case 0:
				/*
				 * case 3: timeDiff is a small or medium-sized
				 * negative num, eg. because of DST ending just run
				 * the wildcard jobs. The fixed-time jobs probably
				 * have already run, and should not be repeated
				 * virtual time does not change until we are caught up
				 */
				Debug(DSCH, ("[%d], DST ends %d minutes to go\n",
				    getpid(), virtualTime - timeRunning))
				find_jobs(timeRunning, &database, TRUE, FALSE);
				break;
			default:
				/*
				 * other: time has changed a *lot*,
				 * jump virtual time, and run everything
				 */
				Debug(DSCH, ("[%d], clock jumped\n", getpid()))
				virtualTime = timeRunning;
				find_jobs(timeRunning, &database, TRUE, TRUE);
			}
		}
		/* jobs to be run (if any) are loaded. clear the queue */
		job_runqueue();
	}
}

#ifdef DEBIAN
#include <sys/stat.h>
#include <fcntl.h>
#endif

static void
run_reboot_jobs(db)
	cron_db *db;
{
	register user		*u;
	register entry		*e;
    int rbfd;
#ifdef DEBIAN
#define REBOOT_FILE "/var/run/crond.reboot"
	/* Run on actual reboot, rather than cron restart */
	if (access(REBOOT_FILE, F_OK) == 0) {
	        /* File exists, return */
     	        log_it("CRON", getpid(),"INFO",
		       "Skipping @reboot jobs -- not system startup");
	        return;
	}
	/* Create the file */
	if ((rbfd = creat(REBOOT_FILE, S_IRUSR&S_IWUSR)) < 0) {
		/* Bad news, bail out */
	        log_it("CRON",getpid(),"DEATH","Can't create reboot check file");
		exit(0);
	} else {
		close(rbfd);
		log_it("CRON", getpid(),"INFO", "Running @reboot jobs");
	}
      

        Debug(DMISC, ("[%d], Debian running reboot jobs\n",getpid()));
    
#endif
        Debug(DMISC, ("[%d], vixie running reboot jobs\n", getpid()));
	for (u = db->head;  u != NULL;  u = u->next) {
		for (e = u->crontab;  e != NULL;  e = e->next) {
			if (e->flags & WHEN_REBOOT) {
				job_add(e, u);
			}
		}
	}
	(void) job_runqueue();
}


static void
find_jobs(vtime, db, doWild, doNonWild)
	time_min vtime;
	cron_db	*db;
	int doWild;
	int doNonWild;
{
	time_t   virtualSecond  = vtime * SECONDS_PER_MINUTE;
	register struct tm 	*tm = gmtime(&virtualSecond);
	register int		minute, hour, dom, month, dow;
	register user		*u;
	register entry		*e;

	/* make 0-based values out of these so we can use them as indicies
	 */
	minute = tm->tm_min -FIRST_MINUTE;
	hour = tm->tm_hour -FIRST_HOUR;
	dom = tm->tm_mday -FIRST_DOM;
	month = tm->tm_mon +1 /* 0..11 -> 1..12 */ -FIRST_MONTH;
	dow = tm->tm_wday -FIRST_DOW;

	Debug(DSCH, ("[%d] tick(%d,%d,%d,%d,%d) %s %s\n",
		getpid(), minute, hour, dom, month, dow,
		doWild?" ":"No wildcard",doNonWild?" ":"Wildcard only"))

	/* the dom/dow situation is odd.  '* * 1,15 * Sun' will run on the
	 * first and fifteenth AND every Sunday;  '* * * * Sun' will run *only*
	 * on Sundays;  '* * 1,15 * *' will run *only* the 1st and 15th.  this
	 * is why we keep 'e->dow_star' and 'e->dom_star'.  yes, it's bizarre.
	 * like many bizarre things, it's the standard.
	 */
	for (u = db->head;  u != NULL;  u = u->next) {
		for (e = u->crontab;  e != NULL;  e = e->next) {
			Debug(DSCH|DEXT, ("user [%s:%d:%d:...] cmd=\"%s\"\n",
			    env_get("LOGNAME", e->envp),
			    e->uid, e->gid, e->cmd))
			if (bit_test(e->minute, minute) &&
			    bit_test(e->hour, hour) &&
			    bit_test(e->month, month) &&
			    ( ((e->flags & DOM_STAR) || (e->flags & DOW_STAR))
			      ? (bit_test(e->dow,dow) && bit_test(e->dom,dom))
			      : (bit_test(e->dow,dow) || bit_test(e->dom,dom)))) {
				if ((doNonWild && !(e->flags & (MIN_STAR|HR_STAR)))
				    || (doWild && (e->flags & (MIN_STAR|HR_STAR))))
					job_add(e, u);
			}
		}
	}
}


/*
 * Set StartTime and clockTime to the current time.
 * These are used for computing what time it really is right now.
 * Note that clockTime is a unix wallclock time converted to minutes.
 */
static void
set_time(int initialize)
{
    struct tm tm;
    static int isdst;

    StartTime = time(NULL);

    /* We adjust the time to GMT so we can catch DST changes. */
    tm = *localtime(&StartTime);
    if (initialize || tm.tm_isdst != isdst) {
       isdst = tm.tm_isdst;
       GMToff = get_gmtoff(&StartTime, &tm);
       Debug(DSCH, ("[%d] GMToff=%ld\n",
           getpid(), (long)GMToff))
    }
    clockTime = (StartTime + GMToff) / (time_t)SECONDS_PER_MINUTE;
}

/*
 * try to just hit the next minute
 */
static void
cron_sleep(target)
	time_min target;
{
	time_t t;
	int seconds_to_wait;

	t = time(NULL) + GMToff;

	seconds_to_wait = (int)(target * SECONDS_PER_MINUTE - t) + 1;
	Debug(DSCH, ("[%d] TargetTime=%ld, sec-to-wait=%d\n",
	    getpid(), (long)target*SECONDS_PER_MINUTE, seconds_to_wait))

        if (seconds_to_wait > 0 && seconds_to_wait < 65)
            sleep((unsigned int) seconds_to_wait);
}


#ifdef USE_SIGCHLD
static void
sigchld_handler(x) {
	int save_errno = errno;
	WAIT_T		waiter;
	PID_T		pid;

	for (;;) {
#ifdef POSIX
		pid = waitpid(-1, &waiter, WNOHANG);
#else
		pid = wait3(&waiter, WNOHANG, (struct rusage *)0);
#endif
		switch (pid) {
		case -1:
			Debug(DPROC,
				("[%d] sigchld...no children\n", getpid()))
			errno = save_errno;
			return;
		case 0:
			Debug(DPROC,
				("[%d] sigchld...no dead kids\n", getpid()))
			errno = save_errno;
			return;
		default:
			Debug(DPROC,
				("[%d] sigchld...pid #%d died, stat=%d\n",
				getpid(), pid, WEXITSTATUS(waiter)))
		}
	}
	errno = save_errno;
}
#endif /*USE_SIGCHLD*/


static void
sighup_handler(x) {
	log_close();

	/* we should use sigaction for proper signal blocking as this 
	   has a race, but... */
	signal(SIGHUP, sighup_handler);
}


static void
parse_args(argc, argv)
	int	argc;
	char	*argv[];
{
	int	argch;

	log_level = 1;
	stay_foreground = 1;
        lsbsysinit_mode = 0;
        fqdn_in_subject = 0;

	while (EOF != (argch = getopt(argc, argv, "lfnx:L:"))) {
		switch (argch) {
		default:
			usage();
		case 'f':
			stay_foreground = 1;
			break;
		case 'x':
			if (!set_debug_flags(optarg))
				usage();
			break;
		case 'l':
			lsbsysinit_mode = 1;
			break;
		case 'n':
			fqdn_in_subject = 1;
			break;
		case 'L':
			log_level = atoi(optarg);
			break;
		}
	}
}
