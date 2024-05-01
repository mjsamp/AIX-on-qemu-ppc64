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


/* vix 26jan87 [RCS has the log]
 */


#define __USE_GNU /* For O_NOFOLLOW */
#include <fcntl.h>
#undef __USE_GNU
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>

#define TMAX(a,b) ((a)>(b)?(a):(b))

/* Try to get maximum path name -- this isn't really correct, but we're
going to be lazy */

#ifndef PATH_MAX

#ifdef MAXPATHLEN
#define PATH_MAX MAXPATHLEN 
#else
#define PATH_MAX 2048
#endif

#endif /* ifndef PATH_MAX */

static	void		process_crontab __P((char *, char *, char *,
					     struct stat *,
					     cron_db *, cron_db *));
#ifdef DEBIAN
static int valid_name (char *filename);
static user *get_next_system_crontab __P((user *));
#endif

void force_rescan_user(cron_db *old_db, cron_db *new_db, const char *fname, time_t old_mtime);

static void add_orphan(const char *uname, const char *fname, const char *tabname);
static void free_orphan(orphan *o);

void
load_database(old_db)
	cron_db		*old_db;
{
        DIR		*dir;
	struct stat	statbuf;
	struct stat	syscron_stat;
	DIR_T   	*dp;
	cron_db		new_db;
	user		*u, *nu;
#ifdef DEBIAN
	struct stat     syscrond_stat;
	struct stat     syscrond_file_stat;
	
        char            syscrond_fname[PATH_MAX+1];
	int             syscrond_change = 0;
#endif

	Debug(DLOAD, ("[%d] load_database()\n", getpid()))

	/* before we start loading any data, do a stat on SPOOL_DIR
	 * so that if anything changes as of this moment (i.e., before we've
	 * cached any of the database), we'll see the changes next time.
	 */
	if (stat(SPOOL_DIR, &statbuf) < OK) {
		log_it("CRON", getpid(), "STAT FAILED", SPOOL_DIR);
		statbuf.st_mtime = 0;
	}

	/* track system crontab file
	 */
	if (stat(SYSCRONTAB, &syscron_stat) < OK) {
		log_it("CRON", getpid(), "STAT FAILED", SYSCRONTAB);
		syscron_stat.st_mtime = 0;
	}

#ifdef DEBIAN
	/* Check mod time of SYSCRONDIR. This won't tell us if a file
         * in it changed, but will capture deletions, which the individual
         * file check won't
	 */
	if (stat(SYSCRONDIR, &syscrond_stat) < OK) {
		log_it("CRON", getpid(), "STAT FAILED", SYSCRONDIR);
		syscrond_stat.st_mtime = 0;
	}

	/* If SYSCRONDIR was modified, we know that something is changed and
	 * there is no need for any further checks. If it wasn't, we should
	 * pass through the old list of files in SYSCRONDIR and check their
	 * mod time. Therefore a stopped hard drive won't be spun up, since
	 * we avoid reading of SYSCRONDIR and don't change its access time.
	 * This is especially important on laptops with APM.
	 */
	if (old_db->sysd_mtime != syscrond_stat.st_mtime) {
	        syscrond_change = 1;
	} else {
	        /* Look through the individual files */
		user *systab;

		Debug(DLOAD, ("[%d] system dir mtime unch, check files now.\n",
			      getpid()))

		for (systab = old_db->head;
		     (systab = get_next_system_crontab (systab)) != NULL;
		     systab = systab->next) {

			sprintf(syscrond_fname, "%s/%s", SYSCRONDIR,
							 systab->name + 8);

			Debug(DLOAD, ("\t%s:", syscrond_fname))

			if (stat(syscrond_fname, &syscrond_file_stat) < OK)
				syscrond_file_stat.st_mtime = 0;

			if (syscrond_file_stat.st_mtime != systab->mtime ||
				systab->mtime == 0) {
			        syscrond_change = 1;
                        }

			Debug(DLOAD, (" [checked]\n"))
		}
	}
#endif /* DEBIAN */

	/* if spooldir's mtime has not changed, we don't need to fiddle with
	 * the database.
	 *
	 * Note that old_db->mtime is initialized to 0 in main(), and
	 * so is guaranteed to be different than the stat() mtime the first
	 * time this function is called.
	 */
#ifdef DEBIAN
	if ((old_db->user_mtime == statbuf.st_mtime) &&
	    (old_db->sys_mtime == syscron_stat.st_mtime) &&
	    (!syscrond_change)) {
#else
	if ((old_db->user_mtime == statbuf.st_mtime) &&
	    (old_db->sys_mtime == syscron_stat.st_mtime)) {
#endif
		Debug(DLOAD, ("[%d] spool dir mtime unch, no load needed.\n",
			      getpid()))
		return;
	}

	/* something's different.  make a new database, moving unchanged
	 * elements from the old database, reloading elements that have
	 * actually changed.  Whatever is left in the old database when
	 * we're done is chaff -- crontabs that disappeared.
	 */
	new_db.user_mtime = statbuf.st_mtime;
	new_db.sys_mtime = syscron_stat.st_mtime;
#ifdef DEBIAN
	new_db.sysd_mtime = syscrond_stat.st_mtime;
#endif
	new_db.head = new_db.tail = NULL;

	if (syscron_stat.st_mtime) {
		process_crontab(SYSUSERNAME, "*system*",
				SYSCRONTAB, &syscron_stat,
				&new_db, old_db);
	}

#ifdef DEBIAN
	/* Read all the package crontabs. */
	if (!(dir = opendir(SYSCRONDIR))) {
		log_it("CRON", getpid(), "OPENDIR FAILED", SYSCRONDIR);
	}

	while (dir != NULL && NULL != (dp = readdir(dir))) {
		char	fname[MAXNAMLEN+1],
		        tabname[PATH_MAX+1];


		/* avoid file names beginning with ".".  this is good
		 * because we would otherwise waste two guaranteed calls
		 * to stat() for . and .., and also because package names
		 * starting with a period are just too nasty to consider.
		 */
		if (dp->d_name[0] == '.')
			continue;

		/* skipfile names with letters outside the set
		 * [A-Za-z0-9_-], like run-parts.
		 */
		if (!valid_name(dp->d_name))
		  continue;

		/* Generate the "fname" */
		(void) strcpy(fname,"*system*");
		(void) strcat(fname, dp->d_name);
		sprintf(tabname,"%s/%s", SYSCRONDIR, dp->d_name);

		/* statbuf is used as working storage by process_crontab() --
		   current contents are irrelevant */
		process_crontab(SYSUSERNAME, fname, tabname,
				&statbuf, &new_db, old_db);

	}
	if (dir)
		closedir(dir);
#endif

	/* we used to keep this dir open all the time, for the sake of
	 * efficiency.  however, we need to close it in every fork, and
	 * we fork a lot more often than the mtime of the dir changes.
	 */
	if (!(dir = opendir(SPOOL_DIR))) {
		log_it("CRON", getpid(), "OPENDIR FAILED", SPOOL_DIR);
	}

	while (dir != NULL && NULL != (dp = readdir(dir))) {
		char	fname[MAXNAMLEN+1],
			tabname[PATH_MAX+1];

		/* avoid file names beginning with ".".  this is good
		 * because we would otherwise waste two guaranteed calls
		 * to getpwnam() for . and .., and also because user names
		 * starting with a period are just too nasty to consider.
		 */
		if (dp->d_name[0] == '.')
			continue;

		(void) strcpy(fname, dp->d_name);
		snprintf(tabname, PATH_MAX+1, CRON_TAB(fname));

		process_crontab(fname, fname, tabname,
				&statbuf, &new_db, old_db);
	}
	if (dir)
		closedir(dir);

	/* if we don't do this, then when our children eventually call
	 * getpwnam() in do_command.c's child_process to verify MAILTO=,
	 * they will screw us up (and v-v).
	 */
	endpwent();

	/* whatever's left in the old database is now junk.
	 */
	Debug(DLOAD, ("unlinking old database:\n"))
	for (u = old_db->head;  u != NULL;  u = nu) {
		Debug(DLOAD, ("\t%s\n", u->name))
		nu = u->next;
		unlink_user(old_db, u);
		free_user(u);
	}

	/* overwrite the database control block with the new one.
	 */
	*old_db = new_db;
	Debug(DLOAD, ("load_database is done\n"))
}


void
link_user(db, u)
	cron_db	*db;
	user	*u;
{
	if (db->head == NULL)
		db->head = u;
	if (db->tail)
		db->tail->next = u;
	u->prev = db->tail;
	u->next = NULL;
	db->tail = u;
}


void
unlink_user(db, u)
	cron_db	*db;
	user	*u;
{
	if (u->prev == NULL)
		db->head = u->next;
	else
		u->prev->next = u->next;

	if (u->next == NULL)
		db->tail = u->prev;
	else
		u->next->prev = u->prev;
}


user *
find_user(db, name)
	cron_db	*db;
	char	*name;
{
	char	*env_get();
	user	*u;

	for (u = db->head;  u != NULL;  u = u->next)
		if (!strcmp(u->name, name))
			break;
	return u;
}


static void
process_crontab(uname, fname, tabname, statbuf, new_db, old_db)
	char		*uname;
	char		*fname;
	char		*tabname;
	struct stat	*statbuf;
	cron_db		*new_db;
	cron_db		*old_db;
{
	struct passwd	*pw = NULL;
	int		crontab_fd = OK - 1;
	user		*u = NULL;

#ifdef DEBIAN
	/* If the name begins with *system*, don't worry about password -
	 it's part of the system crontab */
	if (strncmp(fname, "*system*", 8) && !(pw = getpwnam(uname))) {
#else
	if (strcmp(fname, "*system*") && !(pw = getpwnam(uname))) {
#endif
		/* file doesn't have a user in passwd file.
		 */
		if (strncmp(fname, "tmp.", 4)) {
			/* don't log these temporary files */
			log_it(fname, getpid(), "ORPHAN", "no passwd entry");
			add_orphan(uname, fname, tabname);
		}
		goto next_crontab;
	}

        if (pw) {
            /* Path for user crontabs (including root's!) */
            if ((crontab_fd = open(tabname, O_RDONLY|O_NOFOLLOW, 0)) < OK) {
		/* crontab not accessible?
		 */
		log_it(fname, getpid(), "CAN'T OPEN", tabname);
		goto next_crontab;
            }

            if (fstat(crontab_fd, statbuf) < OK) {
		log_it(fname, getpid(), "FSTAT FAILED", tabname);
		goto next_crontab;
            }
            /* Check to make sure that the crontab is owned by the correct user
               (or root) */
            if (statbuf->st_uid != pw->pw_uid && statbuf->st_uid != ROOT_UID) {
                log_it(fname, getpid(), "WRONG FILE OWNER", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
            }

	    /* Check to make sure that the crontab is a regular file */
            if (!S_ISREG(statbuf->st_mode)) {
		log_it(fname, getpid(), "NOT A REGULAR FILE", tabname);
		goto next_crontab;
	    }

	    /* Check to make sure that the crontab's permissions are secure */
            if ((statbuf->st_mode & 07777) != 0600) {
		log_it(fname, getpid(), "INSECURE MODE (mode 0600 expected)", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
	    }

	    /* Check to make sure that there are no hardlinks to the crontab */
            if (statbuf->st_nlink != 1) {
		log_it(fname, getpid(), "NUMBER OF HARD LINKS > 1", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
	    }
        } else {
            /* System crontab path. These can be symlinks, but the
               symlink and the target must be owned by root. */
            if (lstat(tabname, statbuf) < OK) {
		log_it(fname, getpid(), "LSTAT FAILED", tabname);
		goto next_crontab;
            }
            if (S_ISLNK(statbuf->st_mode) && statbuf->st_uid != ROOT_UID) {
                log_it(fname, getpid(), "WRONG SYMLINK OWNER", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
            }
            if ((crontab_fd = open(tabname, O_RDONLY, 0)) < OK) {
		/* crontab not accessible?

		   If tabname is a symlink, it's most probably just broken, so
		   we force a rescan. Once the link is fixed, it will get picked
		   up and processed again. If tabname is a regular file, this
		   error is bad so we skip it instead.
		 */
		if (S_ISLNK(statbuf->st_mode)) {
                    log_it(fname, getpid(), "CAN'T OPEN SYMLINK", tabname);
                    force_rescan_user(old_db, new_db, fname, 0);
                    goto next_crontab;
                } else {
		    log_it(fname, getpid(), "CAN'T OPEN", tabname);
		    goto next_crontab;
		}
            }

            if (fstat(crontab_fd, statbuf) < OK) {
		log_it(fname, getpid(), "FSTAT FAILED", tabname);
		goto next_crontab;
            }

            /* Check to make sure that the crontab is owned by root */
            if (statbuf->st_uid != ROOT_UID) {
                log_it(fname, getpid(), "WRONG FILE OWNER", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
            }

            /* Check to make sure that the crontab is a regular file */
            if (!S_ISREG(statbuf->st_mode)) {
		log_it(fname, getpid(), "NOT A REGULAR FILE", tabname);
		goto next_crontab;
	    }

            /* Check to make sure that the crontab is writable only by root
	     * This should really be in sync with the check for users above
	     * (mode 0600). An upgrade path could be implemented for 4.1
	     */
	    if ((statbuf->st_mode & S_IWGRP) || (statbuf->st_mode & S_IWOTH)) {
		log_it(fname, getpid(), "INSECURE MODE (group/other writable)", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
	    }
            /* Technically, we should also check whether the parent dir is
 	     * writable, and so on. This would only make proper sense for
 	     * regular files; we can't realistically check all possible
 	     * security issues resulting from symlinks. We'll just assume that
 	     * root will handle responsible when creating them.
	     */

	    /* Check to make sure that there are no hardlinks to the crontab */
            if (statbuf->st_nlink != 1) {
		log_it(fname, getpid(), "NUMBER OF HARD LINKS > 1", tabname);
                force_rescan_user(old_db, new_db, fname, 0);
		goto next_crontab;
	    }
        }
        /*
         * The link count check is not sufficient (the owner may
         * delete their original link, reducing the link count back to
         * 1), but this is all we've got.
         */
	Debug(DLOAD, ("\t%s:", fname))

	if (old_db != NULL)
		u = find_user(old_db, fname);

	if (u != NULL) {
		/* if crontab has not changed since we last read it
		 * in, then we can just use our existing entry.
		 */
		if (u->mtime == statbuf->st_mtime) {
			Debug(DLOAD, (" [no change, using old data]"))
			unlink_user(old_db, u);
			link_user(new_db, u);
			goto next_crontab;
		}

		/* before we fall through to the code that will reload
		 * the user, let's deallocate and unlink the user in
		 * the old database.  This is more a point of memory
		 * efficiency than anything else, since all leftover
		 * users will be deleted from the old database when
		 * we finish with the crontab...
		 */
		Debug(DLOAD, (" [delete old data]"))
		unlink_user(old_db, u);
		free_user(u);
		log_it(fname, getpid(), "RELOAD", tabname);
	}

	u = load_user(crontab_fd, pw, uname, fname, tabname);
	if (u != NULL) {
		u->mtime = statbuf->st_mtime;
		link_user(new_db, u);
        } else {
                /* The crontab we attempted to load contains a syntax error. A
                 * fix won't get picked up by the regular change detection
                 * code, so we force a rescan. statbuf->st_mtime still contains
                 * the file's mtime, so we use it to rescan only when an update
                 * has actually taken place.
                 */
                force_rescan_user(old_db, new_db, fname, statbuf->st_mtime);
        }   


next_crontab:
	if (crontab_fd >= OK) {
		Debug(DLOAD, (" [done]\n"))
		close(crontab_fd);
	}
}

#ifdef DEBIAN

#include <regex.h>

/* True or false? Is this a valid filename? */

/* Taken from Clint Adams 'run-parts' version to support lsb style
   names, originally GPL, but relicensed to cron license per e-mail of
   27 September 2003. I've changed it to do regcomp() only once. */

static int
valid_name(char *filename)
{
  static regex_t hierre, tradre, excsre, classicalre;
  static int donere = 0;

  if (!donere) {
      donere = 1;
      if (regcomp(&hierre, "^_?([a-z0-9_.]+-)+[a-z0-9]+$",
                  REG_EXTENDED | REG_NOSUB)
          || regcomp(&excsre, "^[a-z0-9-].*dpkg-(old|dist)$",
                     REG_EXTENDED | REG_NOSUB)
          || regcomp(&tradre, "^[a-z0-9][a-z0-9-]*$", REG_NOSUB)
          || regcomp(&classicalre, "^[a-zA-Z0-9_-]+$",
                     REG_EXTENDED | REG_NOSUB)) {
          log_it("CRON", getpid(), "REGEX FAILED", "valid_name");
          (void) exit(ERROR_EXIT);
      }
  }
  if (lsbsysinit_mode) {
      if (!regexec(&hierre, filename, 0, NULL, 0)) {
          return regexec(&excsre, filename, 0, NULL, 0);
      } else {
          return !regexec(&tradre, filename, 0, NULL, 0);
      }
  }
  /* Old standard style */
  return !regexec(&classicalre, filename, 0, NULL, 0);
}


static user *
get_next_system_crontab (curtab)
	user	*curtab;
{
	for ( ; curtab != NULL; curtab = curtab->next)
		if (!strncmp(curtab->name, "*system*", 8) && curtab->name [8])
			break;
	return curtab;
}

#endif

/* Force rescan of a crontab the next time cron wakes up
 *
 * cron currently only detects changes caused by an mtime update; it does not
 * detect other attribute changes such as UID or mode. To allow cron to recover
 * from errors of that nature as well, this function removes the crontab from
 * the old DB (if present there) and adds an empty crontab to the new DB with
 * a given mtime. Specifying mtime as 0 will force a rescan the next time the
 * daemon wakes up.
 */
void
force_rescan_user(cron_db *old_db, cron_db *new_db, const char *fname, time_t old_mtime)
{
        user *u;

	/* Remove from old DB and free resources */
	u = find_user(old_db, fname);
	if (u != NULL) {
		Debug(DLOAD, (" [delete old data]"))
		unlink_user(old_db, u);
		free_user(u);
	}

	/* Allocate an empty crontab with the specified mtime, add it to new DB */
        if ((u = (user *) malloc(sizeof(user))) == NULL) {
                errno = ENOMEM;
        }   
        if ((u->name = strdup(fname)) == NULL) {
                free(u);
                errno = ENOMEM;
        }   
        u->mtime = old_mtime;
        u->crontab = NULL;
#ifdef WITH_SELINUX
        u->scontext = NULL;
#endif
        Debug(DLOAD, ("\t%s: [added empty placeholder to force rescan]\n", fname))
	link_user(new_db, u);
}

/* This fix was taken from Fedora cronie */
static orphan *orphans;

static void
free_orphan(orphan *o) {
        free(o->tabname);
        free(o->fname);
        free(o->uname);
        free(o);
}

void
check_orphans(cron_db *db) {
        orphan *prev_orphan = NULL;
        orphan *o = orphans;
	struct stat statbuf;

        while (o != NULL) {
                if (getpwnam(o->uname) != NULL) {
                        orphan *next = o->next;

                        if (prev_orphan == NULL) {
                                orphans = next;
                        } else {
                                prev_orphan->next = next;
                        }   

                        process_crontab(o->uname, o->fname, o->tabname,
                                &statbuf, db, NULL);

                        /* process_crontab could have added a new orphan */
                        if (prev_orphan == NULL && orphans != next) {
                                prev_orphan = orphans;
                        }   
                        free_orphan(o);
                        o = next;
                } else {
                        prev_orphan = o;
                        o = o->next;
                }   
        }   
}

static void
add_orphan(const char *uname, const char *fname, const char *tabname) {
        orphan *o; 

        o = calloc(1, sizeof(*o));
        if (o == NULL)
                return;

        if (uname)
                if ((o->uname=strdup(uname)) == NULL)
                        goto cleanup;

        if (fname)
                if ((o->fname=strdup(fname)) == NULL)
                        goto cleanup;

        if (tabname)
                if ((o->tabname=strdup(tabname)) == NULL)
                        goto cleanup;

        o->next = orphans;
        orphans = o;
        return;

cleanup:
        free_orphan(o);
}
