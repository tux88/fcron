
/*
 * FCRON - periodic command scheduler 
 *
 *  Copyright 2000-2001 Thibault Godouet <fcron@free.fr>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 *  The GNU General Public License can also be found in the file
 *  `LICENSE' that comes with the fcron source distribution.
 */

 /* $Id: fcrontab.c,v 1.37 2001-05-24 19:54:48 thib Exp $ */

/* 
 * The goal of this program is simple : giving a user interface to fcron
 * daemon, by allowing each user to see, modify, append or remove his 
 * fcrontabs. 
 * Fcron daemon use a specific formated format of file, so fcrontab generate
 * that kind of file from human readable files. In order allowing users to
 * see and modify their fcrontabs, the source file is always saved with the
 * formated one. 
 * Fcrontab makes a temporary formated file, and then sends a signal 
 * to the daemon to force it to update its configuration, remove the temp
 * file and save a new and final formated file.
 * That way, not the simple, allows the daemon to keep a maximum of 
 * informations like the time remaining before next execution, or the date
 * and hour of next execution.
 */

#include "fcrontab.h"

char rcs_info[] = "$Id: fcrontab.c,v 1.37 2001-05-24 19:54:48 thib Exp $";

void info(void);
void usage(void);
void sig_daemon(void);
pid_t read_pid(void);


/* used in temp_file() */
char *tmp_path = "/tmp/";

/* command line options */
char rm_opt = 0;
char list_opt = 0;
char edit_opt = 0;
char reinstall_opt = 0;
char ignore_prev = 0;
int file_opt = 0;

#ifdef DEBUG
char debug_opt = 1;       /* set to 1 if we are in debug mode */
#else
char debug_opt = 0;       /* set to 1 if we are in debug mode */
#endif

char *user = NULL;
uid_t uid = 0;
uid_t asuid = 0;
gid_t asgid = 0;
uid_t fcrontab_uid = 0;
gid_t fcrontab_gid = 0;

char need_sig = 0;           /* do we need to signal fcron daemon */

char *orig_dir = NULL;
CF *file_base = NULL;
char buf[FNAME_LEN];
char file[FNAME_LEN];

/* needed by log part : */
char *prog_name = NULL;
char foreground = 1;
pid_t daemon_pid = 0;


void
info(void)
    /* print some informations about this program :
     * version, license */
{
    fprintf(stderr,
	    "fcrontab " VERSION_QUOTED " - user interface to daemon fcron\n"
	    "Copyright 2000-2001 Thibault Godouet <fcron@free.fr>\n"
	    "This program is free software distributed WITHOUT ANY WARRANTY.\n"
            "See the GNU General Public License for more details.\n"
	);

    exit(EXIT_OK);

}


void
usage(void)
  /*  print a help message about command line options and exit */
{
    fprintf(stderr, 
	    "fcrontab [-n] file [user|-u user]\n"
	    "fcrontab { -l | -r | -e | -z } [-n] [user|-u user]\n"
	    "fcrontab -h\n"
	    "  -u user    specify user name.\n"
	    "  -l         list user's current fcrontab.\n"
	    "  -r         remove user's current fcrontab.\n"
	    "  -e         edit user's current fcrontab.\n"
	    "  -z         reinstall user's fcrontab from source code.\n"
	    "  -n         ignore previous version of file.\n"
	    "  -d         set up debug mode.\n"
	    "  -h         display this help message.\n"
	    "\n"
	);
    
    exit(EXIT_ERR);
}


pid_t
read_pid(void)
    /* return fcron daemon's pid if running.
     * otherwise return 0 */
{
    FILE *fp = NULL;
    pid_t pid = 0;
    
    if ((fp = fopen(PIDFILE, "r")) != NULL) {
	fscanf(fp, "%d", &pid);    
	fclose(fp);
    }

    return pid;
}


void
sig_daemon(void)
    /* send SIGHUP to daemon to tell him configuration has changed */
    /* SIGHUP is sent once 10s before the next minute to avoid
     * some bad users to block daemon by sending it SIGHUP all the time */
{
    /* we don't need to make root wait */
    if (uid != 0) {
	time_t t = 0;
	int sl = 0;
	FILE *fp = NULL;
	int	fd = 0;
	struct tm *tm = NULL;


	t = time(NULL);
	tm = localtime(&t);
    
	if ( (sl = 60 - (t % 60) - 10) < 0 ) {
	    if ( (tm->tm_min = tm->tm_min + 2) >= 60 ) {
		tm->tm_hour++;
		tm->tm_min -= 60;
	    }
	    snprintf(buf, sizeof(buf), "%02dh%02d", tm->tm_hour, tm->tm_min);
	    sl = 60 - (t % 60) + 50;
	} else {
	    if ( ++tm->tm_min >= 60 ) {
		tm->tm_hour++;
		tm->tm_min -= 60;
	    }
	    snprintf(buf, sizeof(buf), "%02dh%02d", tm->tm_hour, tm->tm_min);
	}
	fprintf(stderr, "Modifications will be taken into account"
		" at %s.\n", buf);


#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
	if (seteuid(fcrontab_uid) != 0)
	    die_e("seteuid(fcrontab_uid[%d])", fcrontab_uid);
#endif

	/* try to create a lock file */
	if ((fd = open(FCRONTABS "/fcrontab.sig", O_RDWR|O_CREAT, 0644)) == -1
	    || ((fp = fdopen(fd, "r+")) == NULL) )
	    die_e("can't open or create " FCRONTABS "/fcrontab.sig");	
    
	if ( flock(fd, LOCK_EX|LOCK_NB) != 0 ) {
	    debug("fcrontab is already waiting for signalling the daemon :"
		  " exiting.");
	    return;
	}


	(void) fcntl(fd, F_SETFD, 1);

	/* abandon fd and fp even though the file is open. we need to
	 * keep it open and locked, but we don't need the handles elsewhere.
	 */

	switch ( fork() ) {
	case -1:
	    remove(FCRONTABS "/fcrontab.sig");
	    die_e("could not fork : daemon has not been signaled");
	    break;
	case 0:
	    /* child */
	    break;
	default:
	    /* parent */
	    return;
	}

	sleep(sl);
    
	remove(FCRONTABS "/fcrontab.sig");
    }
    else
	fprintf(stderr, "Modifications will be taken into account"
		" right now.\n");

    if ( (daemon_pid = read_pid()) == 0 )
	/* daemon is not running any longer : we exit */
	return ;

    if ( kill(daemon_pid, SIGHUP) != 0)
	die_e("could not send SIGHUP to daemon (pid %d)", daemon_pid);

}



void
xexit(int exit_val)
    /* launch signal if needed and exit */
{
    if ( need_sig == 1 ) {
	/* check if daemon is running */
	if ( (daemon_pid = read_pid()) != 0 )
	    /* warning : we change euid to fcrontab_iud in sig_daemon() */
	    sig_daemon();
	else
	    fprintf(stderr, "fcron is not running :\n  modifications will"
		    " be taken into account at its next execution.\n");
    }

    exit(exit_val);

}


int
copy(char *orig, char *dest)
    /* copy orig file to dest */
{
    FILE *from = NULL, *to = NULL;
    char c;

    if ( (from = fopen(orig, "r")) == NULL) {
	error_e("copy: orig");
	return ERR;
    }
    /* create it as fcrontab_uid (to avoid problem if user's uid changed)
     * except for root. Root requires filesystem uid root for security
     * reasons */
#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
    if (asuid != 0 && seteuid(fcrontab_uid) != 0)
	error_e("seteuid(fcrontab_uid[%d])", fcrontab_uid);
#endif
    if ((to = fopen(dest, "w")) == NULL) {
	error_e("copy: dest");
	return ERR;
    }
#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
    if (asuid != 0 && seteuid(uid) != 0)
	die_e("seteuid(uid[%d])", uid);
#endif
    if (asuid == 0 ) {
	if ( fchmod(fileno(to), S_IWUSR | S_IRUSR) != 0 )
	    error_e("Could not fchmod %s to 600", dest);
	if ( fchown(fileno(to), 0, fcrontab_gid) != 0 )
	    error_e("Could not fchown %s to root", dest);
    }

    while ( (c = getc(from)) != EOF )
	if ( putc(c, to) == EOF ) {
	    error("Error while copying file. Aborting.\n");
	    return ERR;
	}

    fclose(from);
    fclose(to);
    
    return OK;
}


int
remove_fcrontab(char rm_orig)
    /* remove user's fcrontab and tell daemon to update his conf */
    /* note : the binary fcrontab is removed by fcron */
{
    int return_val = OK;
    FILE *f;

    if ( rm_orig )
	explain("removing %s's fcrontab", user);

    /* remove source and formated file */
    if ( (rm_orig && remove(buf)) != 0 ) {
	if ( errno == ENOENT )
	    return_val = ENOENT;
	else
	    error_e("could not remove %s", buf);		
    }

#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
    if (seteuid(fcrontab_uid) != 0)
	error_e("seteuid(fcrontab_uid[%d])", fcrontab_uid);
#endif
	    
    /* try to remove the temp file in case he has not
     * been read by fcron daemon */
    snprintf(buf, sizeof(buf), "new.%s", user);
    remove(buf);

    /* finally create a file in order to tell the daemon
     * a file was removed, and launch a signal to daemon */
    snprintf(buf, sizeof(buf), "rm.%s", user);
    f = fopen(buf, "w");
    fclose(f);
    
    need_sig = 1;

#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
    if (seteuid(uid) != 0)
	die_e("seteuid(uid[%d])", uid);
#endif

    return return_val;

}


int
write_file(char *file)
{
    int return_val = OK;

    if (ignore_prev == 1)
	/* if user wants to ignore previous version, we remove it *
	 * ( fcron daemon remove files no longer wanted before
	 *   adding new ones ) */
	remove_fcrontab(0);

    if ( file_base->cf_line_base == NULL ) {
	/* no entries */
	explain("%s's fcrontab contains no entries : removed.", user);
	remove_fcrontab(0);
    } 
    else {
	/* write the binary fcrontab on disk */
	snprintf(buf, sizeof(buf), "new.%s", user);
	if ( save_file(buf) != OK )
	    return_val = ERR;
    }

    /* copy original file to FCRONTABS dir */
    snprintf(buf, sizeof(buf), "%s.orig", user);
    if ( copy(file, buf) == ERR )
	return_val = ERR;

    return return_val;
}

int
make_file(char *file)
{

    explain("installing file %s for user %s", file, user);

    /* read file and create a list in memory */
    switch ( read_file(file, user) ) {
    case 2:
    case OK:

	if (write_file(file) == ERR)
	    return ERR;

	/* free memory used to store the list */
	delete_file(user);

	/* tell daemon to update the conf */
	need_sig = 1;
	break;

    case ERR:
	return ERR;
    }

    return OK;
    
}


void
list_file(char *file)
{
    FILE *f = NULL;
    char c;

    explain("listing %s's fcrontab", user);

    if ( (f = fopen(file, "r")) == NULL ) {
	if ( errno == ENOENT ) {
	    explain("user %s has no fcrontab.", user);
	    return ;
	}
	else
	    die_e("User %s could not read file \"%s\"", user, file);
    }
    else {

	while ( (c = getc(f)) != EOF )
	    putchar(c);

	fclose(f);

    }

}

void
edit_file(char *buf)
    /* copy file to a temp file, edit that file, and install it
       if necessary */
{
    char *editor = NULL;
    pid_t pid;
    int status;
    struct stat st;
    time_t mtime = 0;
    char *tmp_str;
    FILE *f, *fi;
    int file = 0;
    char c;
    char correction = 0;
    short return_val = EXIT_OK;

    explain("fcrontabs : editing %s's fcrontab", user);	

    if ( (editor = getenv("VISUAL")) == NULL || strcmp(editor, "\0") == 0 )
	if( (editor = getenv("EDITOR")) == NULL || strcmp(editor, "\0") == 0 )
	    editor = EDITOR;
	
    file = temp_file(&tmp_str);
    if ( (fi = fdopen(file, "w")) == NULL ) {
	error_e("could not fdopen");
	goto exiterr;
    }
#if ! (defined(HAVE_SETREGID) && defined(HAVE_SETREUID))
    if (fchown(file, asuid, asgid) != 0) {
	error_e("Could not fchown %s to asuid and asgid", tmp_str);
	goto exiterr;
    }
#endif
    /* copy user's fcrontab (if any) to a temp file */
    if ( (f = fopen(buf, "r")) == NULL ) {
	if ( errno != ENOENT ) {
	    error_e("could not open file %s", buf);
	    goto exiterr;
	}
	else
	    fprintf(stderr, "no fcrontab for %s - using an empty one\n",
		    user);
    }
    else { 
	/* copy original file to temp file */
	while ( (c=getc(f)) != EOF )
	    putc(c, fi);
	fclose(f);
    }

    fclose(fi);
    close(file);

    do {

	if ( stat(tmp_str, &st) == 0 )
	    mtime = st.st_mtime;
	else {
	    error_e("could not stat \"%s\"", buf);
	    goto exiterr;
	}

	switch ( pid = fork() ) {
	case 0:
	    /* child */
#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
	    if (uid != 0) {
		if (setregid(asgid, asgid) < 0) {
		    error_e("setregid(asgid, asgid)");
		    goto exiterr;
		}
		if (setreuid(asuid, asuid) < 0) {
		    error_e("setreuid(asuid, asuid)");
		    goto exiterr;
		}
	    }
#else
  	    if (setgid(asgid) < 0) {
  		error_e("setgid(asgid)");
  		goto exiterr;
  	    }
	    if (setuid(asuid) < 0) {
		error_e("setuid(asuid)");
		goto exiterr;
	    }
#endif
	    execlp(editor, editor, tmp_str, NULL);
	    error_e("Error while running \"%s\"", editor);
	    goto exiterr;

	case -1:
	    error_e("fork");
	    goto exiterr;

	default:
	    /* parent */
	    break ;
	}
	    
	/* only reached by parent */
	wait4(pid, &status, 0, NULL);
	if ( ! WIFEXITED(status) ) {
	    fprintf(stderr, "Editor exited abnormally:"
		    " fcrontab is unchanged.\n");
	    goto exiterr;
	}

#if ! (defined(HAVE_SETREGID) && defined(HAVE_SETREUID))
	/* we have chown the tmp file to user's name : user may have
	 * linked the tmp file to a file owned by root. In that case, as
	 * fcrontab is setuid root, user may read some informations he is not
	 * allowed to read :
	 * we *must* check that the tmp file is not a link. */

	/* open the tmp file, chown it to root and chmod it to avoid race
	 * conditions */
	/* make sure that the tmp file is not a link */
	{
	    int fd = 0;
	    if ( (fd = open(tmp_str, O_RDONLY)) <= 0 ||
		 fstat(fd, &st) != 0 || ! S_ISREG(st.st_mode) ||
		 S_ISLNK(st.st.mode) || st.st_uid != asuid || st.st_nlink > 1){
		fprintf(stderr, "%s is not a valid regular file.\n", tmp_str);
		close(fd);
		goto exiterr;
	    }
	    if ( fchown(fd, 0, 0) != 0 || fchmod(fd, S_IRUSR|S_IWUSR) != 0 ) {
		fprintf(stderr, "Can't chown or chmod %s.\n", tmp_str);
		close(fd);
		goto exiterr;
	    }
	    close(fd);
	}
#endif
	
	/* check if file has been modified */
	if ( stat(tmp_str, &st) != 0 ) {
	    error_e("could not stat %s", tmp_str);
	    goto exiterr;
	}    

	else if ( st.st_mtime > mtime || correction == 1) {

	    correction = 0;

	    switch ( read_file(tmp_str, user) ) {
	    case ERR:
		goto exiterr;
	    case 2:
		fprintf(stderr, "\nFile contains some errors. "
			"Ignore [i] or Correct [c] ? ");
		/* the 2nd getchar() is for the newline char (\n) */
		while ( (c = getchar())	&& c != 'i' && c != 'c' ) {
		    fprintf(stderr, "Please press c to correct, "
			    "or i to ignore: ");
		    while (c != '\n')
			c = getchar();
		}
		if ( c == 'c' ) {
		    /* free memory used to store the list */
		    delete_file(user);
		    correction = 1;
		}
		break;
	    default:
		break;
	    }

	}
	else {
	    fprintf(stderr, "Fcrontab is unchanged :"
		    " no need to install it.\n"); 
	    goto end;
	}

    } while ( correction == 1);

    if ( write_file(tmp_str) != OK )
	return_val = EXIT_ERR;
    
    /* free memory used to store the list */
    delete_file(user);
    
    /* tell daemon to update the conf */
    need_sig = 1;
    
  end:
    if ( remove(tmp_str) != 0 )
	error_e("could not remove %s", tmp_str);
    free(tmp_str);
    xexit (return_val);

  exiterr:
    if ( remove(tmp_str) != 0 )
	error_e("could not remove %s", tmp_str);
    free(tmp_str);
    xexit (EXIT_ERR);

}


int
install_stdin(void)
    /* install what we get through stdin */
{
    int tmp_fd = 0;
    FILE *tmp_file = NULL;
    char *tmp_str = NULL;
    register char c;
    short return_val = EXIT_OK;
	    	    
    tmp_fd = temp_file(&tmp_str);
    
    if( (tmp_file = fdopen(tmp_fd, "w")) == NULL )
	die_e("Could not fdopen file %s", tmp_str);

    while ( (c = getc(stdin)) != EOF )
	putc(c, tmp_file);

    fclose(tmp_file);
    close(tmp_fd);

    if ( make_file(tmp_str) == ERR )
	goto exiterr;
    else
	goto exit;

  exiterr:
	return_val = EXIT_ERR;    
  exit:
    if ( remove(tmp_str) != 0 )
	error_e("Could not remove %s", tmp_str);
    free(tmp_str);
    return return_val;

}

void
reinstall(char *buf)
{
    int i = 0;

    explain("reinstalling %s's fcrontab", user);

    if ( (i = open(buf, O_RDONLY)) < 0) {
	if ( errno == ENOENT ) {
	    fprintf(stderr, "Could not reinstall: user %s has no fcrontab\n",
		    buf);
	}
	else
	    fprintf(stderr, "Could not open \"%s\": %s\n", buf,
		    strerror(errno));

	exit (EXIT_ERR);
    }

    close(0); dup2(i, 0);
    close(i);

    xexit(install_stdin());

}

void
parseopt(int argc, char *argv[])
  /* set options */
{

    char c;
    extern char *optarg;
    extern int optind, opterr, optopt;
    struct passwd *pass;

    /* constants and variables defined by command line */

    while(1) {
	c = getopt(argc, argv, "u:lrezdnhV");
	if (c == EOF) break;
	switch (c) {

	case 'V':
	    info(); break;

	case 'h':
	    usage(); break;

	case 'u':
	    if (getuid() != 0) {
		fprintf(stderr, "must be privileged to use -u\n");
		xexit(EXIT_ERR);
	    }
	    user = strdup2(optarg) ; 
	    break;

	case 'd':
	    debug_opt = 1; break;

	case 'l':
	    if (rm_opt || edit_opt || reinstall_opt) {
		fprintf(stderr, "Only one of the options -l, -r, -e and -z"
			"may be used simultaneously.\n");
		xexit(EXIT_ERR);
	    }
	    list_opt = 1;
	    rm_opt = 0;
	    edit_opt = 0;
	    reinstall_opt = 0;
	    break;

	case 'r':
	    if (list_opt || edit_opt || reinstall_opt) {
		fprintf(stderr, "Only one of the options -l, -r, -e and -z"
			"may be used simultaneously.\n");
		xexit(EXIT_ERR);
	    }
	    list_opt = 0;
	    rm_opt = 1;
	    edit_opt = 0;
	    reinstall_opt = 0;
	    break;

	case 'e':
	    if (list_opt || rm_opt || reinstall_opt) {
		fprintf(stderr, "Only one of the options -l, -r, -e and -z"
			"may be used simultaneously.\n");
		xexit(EXIT_ERR);
	    }
	    list_opt = 0;
	    rm_opt = 0;
	    edit_opt = 1;
	    reinstall_opt = 0;
	    break;

	case 'z':
	    if (list_opt || rm_opt || edit_opt) {
		fprintf(stderr, "Only one of the options -l, -r, -e and -z"
			"may be used simultaneously.\n");
		xexit(EXIT_ERR);
	    }
	    list_opt = 0;
	    rm_opt = 0;
	    edit_opt = 0;
	    reinstall_opt = 1;
	    break;

	case 'n':
	    ignore_prev = 1;
	    break;
	    
	case ':':
	    fprintf(stderr, "(setopt) Missing parameter");
	    usage();

	case '?':
	    usage();

	default:
	    fprintf(stderr, "(setopt) Warning: getopt returned %c", c);
	}
    }

    /* read the file name and/or user and check validity of the arguments */
    if (argc - optind > 2)
	usage();
    else if (argc - optind == 2 ) {
	if ( list_opt + rm_opt + edit_opt + reinstall_opt == 0 )
	    file_opt = optind++;
	else
	    usage();

	if (getuid() != 0) {
	    fprintf(stderr, "must be privileged to use -u\n");
	    xexit(EXIT_ERR);
	}
	free(user);
	user = strdup2(argv[optind]) ; 
    }
    else if (argc - optind == 1) {
	if ( list_opt + rm_opt + edit_opt + reinstall_opt == 0 )
	    file_opt = optind;
	else {
	    if (getuid() != 0) {
		fprintf(stderr, "must be privileged to use [user|-u user]\n");
		xexit(EXIT_ERR);
	    }
	    free(user);
	    user = strdup2(argv[optind]); 	    
	}
    }
    else if (list_opt + rm_opt + edit_opt + reinstall_opt != 1)
	usage();

    if ( user == NULL ) {
	/* get user's name using getpwuid() */
	if ( ! (pass = getpwuid(uid)) )
	    die("user \"%s\" is not in passwd file. Aborting.", USERNAME);
	/* we need to strdup2 the value given by getpwuid() because we free
	 * file->cf_user in delete_file */
	user = strdup2(pass->pw_name);
	asuid = pass->pw_uid;
	asgid = pass->pw_gid;
    }
    else {
	if ( ! (pass = getpwnam(user)) )
	    die("user \"%s\" is not in passwd file. Aborting.", user);
	asuid = pass->pw_uid;
	asgid = pass->pw_gid;
    }

    if ( ! is_allowed(user) ) {
	die("User \"%s\" is not allowed to use %s. Aborting.",
	    user, prog_name);	    
    }
}


int
main(int argc, char **argv)
{

    memset(buf, 0, sizeof(buf));
    memset(file, 0, sizeof(file));

    if (strrchr(argv[0],'/')==NULL) prog_name = argv[0];
    else prog_name = strrchr(argv[0],'/')+1;

    uid = getuid();

    /* get current dir */
    if ( (orig_dir = getcwd(NULL, 0)) == NULL )
	error_e("getcwd");

    /* interpret command line options */
    parseopt(argc, argv);

#if defined(HAVE_SETREGID) && defined(HAVE_SETREUID)
    {
	struct passwd *pass;
	if ( ! (pass = getpwnam(USERNAME)) )
	    die("user \"%s\" is not in passwd file. Aborting.", USERNAME);
	fcrontab_uid = pass->pw_uid;
	fcrontab_gid = pass->pw_gid;

	if (uid != fcrontab_uid)
	    if (seteuid(fcrontab_uid) != 0) 
		die_e("Could not change uid to fcrontab_uid[%d]",fcrontab_uid);
	/* change directory */
	if (chdir(FCRONTABS) != 0) {
	    error_e("Could not chdir to " FCRONTABS );
	    xexit (EXIT_ERR);
	}
	/* get user's permissions */
	if (seteuid(uid) != 0) 
	    die_e("Could not change uid to uid[%d]", uid); 
	if (setegid(fcrontab_gid) != 0) 
	    die_e("Could not change gid to " GROUPNAME "[%d]", fcrontab_gid); 
    }
#else
    if (setuid(0) != 0 ) 
	die_e("Could not change uid to 0"); 
    if (setgid(0) != 0)
    	die_e("Could not change gid to 0");
    /* change directory */
    if (chdir(FCRONTABS) != 0) {
	error_e("Could not chdir to " FCRONTABS );
	xexit (EXIT_ERR);
    }
#endif
    

    /* this program is seteuid : we set default permission mode
     * to  640 for security reasons */
    umask(026);

    snprintf(buf, sizeof(buf), "%s.orig", user);

    /* determine what action should be taken */
    if ( file_opt ) {

	if ( strcmp(argv[file_opt], "-") == 0 )

	    xexit(install_stdin());

	else {

	    if ( *argv[file_opt] != '/' )
		/* this is just the file name, not the path : complete it */
		snprintf(file,sizeof(file),"%s/%s",orig_dir,argv[file_opt]);
	    else
		strncpy(file, argv[file_opt], sizeof(file) - 1);

	    if (make_file(file) == OK)
		xexit(EXIT_OK);
	    else
		xexit(EXIT_ERR);

	}

    } 

    /* remove user's entries */
    if ( rm_opt == 1 ) {

	if ( remove_fcrontab(1) == ENOENT )
	    fprintf(stderr, "no fcrontab for %s\n", user);

	xexit (EXIT_OK);
    }


    /* list user's entries */
    if ( list_opt == 1 ) {

	list_file(buf);

	xexit(EXIT_OK);

    }


    /* edit user's entries */
    if ( edit_opt == 1 ) {

	edit_file(buf);

	xexit(EXIT_OK);

    }

    /* reinstall user's entries */
    if ( reinstall_opt == 1 ) {

	reinstall(buf);

	xexit(EXIT_OK);

    }



    /* never reach */
    return EXIT_OK;
}
