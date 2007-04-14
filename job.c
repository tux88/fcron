/*
 * FCRON - periodic command scheduler 
 *
 *  Copyright 2000-2007 Thibault Godouet <fcron@free.fr>
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

 /* $Id: job.c,v 1.70 2007-04-14 18:04:08 thib Exp $ */

#include "fcron.h"

#include "job.h"

void sig_dfl(void);
void end_job(cl_t *line, int status, FILE *mailf, short mailpos);
void end_mailer(cl_t *line, int status);
#ifdef HAVE_LIBPAM
void die_mail_pame(cl_t *cl, int pamerrno, struct passwd *pas, char *str);
#endif
#define PIPE_READ 0
#define PIPE_WRITE 1
int read_write_pipe(int fd, void *buf, size_t size, int action);
int read_pipe(int fd, void *to, size_t size);
int write_pipe(int fd, void *buf, size_t size);

#ifndef HAVE_SETENV
char env_user[PATH_LEN];
char env_logname[PATH_LEN];
char env_home[PATH_LEN];
char env_shell[PATH_LEN];
char env_tz[PATH_LEN];
#endif

#ifdef WITH_SELINUX
extern char **environ;
#endif

#ifdef HAVE_LIBPAM
void
die_mail_pame(cl_t *cl, int pamerrno, struct passwd *pas, char *str)
/* log an error in syslog, mail user if necessary, and die */
{
    char buf[MAX_MSG];

    strncpy(buf, str, sizeof(buf)-1);
    strncat(buf, " for '%s'", sizeof(buf)-strlen(buf)-1);
    buf[sizeof(buf)-1]='\0';

    if (is_mail(cl->cl_option)) {
	FILE *mailf = create_mail(cl, "Could not run fcron job");

	/* print the error in both syslog and a file, in order to mail it to user */
	if (dup2(fileno(mailf), 1) != 1 || dup2(1, 2) != 2)
	    die_e("dup2() error");    /* dup2 also clears close-on-exec flag */

	foreground = 1;
	error_pame(pamh, pamerrno, buf, cl->cl_shell);
	error("Job '%s' has *not* run.", cl->cl_shell);
	foreground = 0;

	pam_end(pamh, pamerrno);  

	/* Change running state to the user in question : it's safer to run the mail 
	 * as user, not root */
	if (initgroups(pas->pw_name, pas->pw_gid) < 0)
	    die_e("initgroups failed: %s", pas->pw_name);
	if (setgid(pas->pw_gid) < 0) 
	    die("setgid failed: %s %d", pas->pw_name, pas->pw_gid);
	if (setuid(pas->pw_uid) < 0) 
	    die("setuid failed: %s %d", pas->pw_name, pas->pw_uid);

	launch_mailer(cl, mailf);
	/* launch_mailer() does not return : we never get here */
    }
    else
	die_pame(pamh, pamerrno, buf, cl->cl_shell);
}
#endif

int
change_user(struct cl_t *cl)
{
    struct passwd *pas;
#ifdef HAVE_LIBPAM
    int    retcode = 0;
    const char * const * env;
#endif

    /* Obtain password entry and change privileges */

    if ((pas = getpwnam(cl->cl_runas)) == NULL) 
        die("failed to get passwd fields for user \"%s\"", cl->cl_runas);
    
#ifdef HAVE_SETENV
    setenv("USER", pas->pw_name, 1);
    setenv("LOGNAME", pas->pw_name, 1);
    setenv("HOME", pas->pw_dir, 1);
    if (cl->cl_tz != NULL)
	setenv("TZ", cl->cl_tz, 1);
    /* To ensure compatibility with Vixie cron, we don't use the shell defined
     * in /etc/passwd by default, but the default value from fcron.conf instead: */
    if ( *shell == '\0' )
	/* shell is empty, ie. not defined: use value from /etc/passwd */
	setenv("SHELL", pas->pw_shell, 1);
    else
	/* default: use value from fcron.conf */
	setenv("SHELL", shell, 1);
#else
    {
	strcpy(env_user, "USER=");
	strncat(env_user, pas->pw_name, sizeof(env_user)-5-1);
	env_user[sizeof(env_user)-1]='\0';
	putenv( env_user ); 

	strcpy(env_logname, "LOGNAME=");
	strncat(env_logname, pas->pw_name, sizeof(env_logname)-8-1);
	env_logname[sizeof(env_logname)-1]='\0';
	putenv( env_logname ); 

	strcpy(env_home, "HOME=");
	strncat(env_home, pas->pw_dir, sizeof(env_home)-5-1);
	env_home[sizeof(env_home)-1]='\0';
	putenv( env_home );

	if (cl->cl_tz != NULL) {
	    strcpy(env_tz, "TZ=");
	    strncat(env_tz, pas->pw_dir, sizeof(env_tz)-3-1);
	    env_tz[sizeof(env_tz)-1]='\0';
	    putenv( env_tz );
	}

	strcpy(env_shell, "SHELL=");
	/* To ensure compatibility with Vixie cron, we don't use the shell defined
	 * in /etc/passwd by default, but the default value from fcron.conf instead: */
	if ( *shell == '\0' )
	    /* shell is empty, ie. not defined: use value from /etc/passwd */
	    strncat(env_shell, pas->pw_shell, sizeof(env_shell)-6-1);
	else
	    /* default: use value from fcron.conf */
	    strncat(env_shell, shell, sizeof(env_shell)-6-1);
	env_shell[sizeof(env_shell)-1]='\0';
	putenv( env_shell );
    }
#endif /* HAVE_SETENV */

#ifdef HAVE_LIBPAM
    /* Open PAM session for the user and obtain any security
       credentials we might need */

    retcode = pam_start("fcron", pas->pw_name, &apamconv, &pamh);
    if (retcode != PAM_SUCCESS) die_pame(pamh, retcode, "Could not start PAM for %s",
					 cl->cl_shell);
    /* Some system seem to need that pam_authenticate() call.
     * Anyway, we have no way to authentificate the user :
     * we must set auth to pam_permit. */
    retcode = pam_authenticate(pamh, PAM_SILENT);
    if (retcode != PAM_SUCCESS) die_mail_pame(cl, retcode, pas,
					      "Could not authenticate PAM user");
    retcode = pam_acct_mgmt(pamh, PAM_SILENT); /* permitted access? */
    if (retcode != PAM_SUCCESS) die_mail_pame(cl, retcode, pas,
					      "Could not init PAM account management");
    retcode = pam_setcred(pamh, PAM_ESTABLISH_CRED | PAM_SILENT);
    if (retcode != PAM_SUCCESS) die_mail_pame(cl, retcode, pas, 
					      "Could not set PAM credentials");
    retcode = pam_open_session(pamh, PAM_SILENT);
    if (retcode != PAM_SUCCESS) die_mail_pame(cl, retcode, pas,
					      "Could not open PAM session");

    env = (const char * const *) pam_getenvlist(pamh);
    while (env && *env) {
	if (putenv((char*) *env)) die_e("Could not copy PAM environment");
	env++;
    }

    /* Close the log here, because PAM calls openlog(3) and
       our log messages could go to the wrong facility */
    xcloselog();
#endif /* USE_PAM */

    /* Change running state to the user in question */
    if (initgroups(pas->pw_name, pas->pw_gid) < 0)
	die_e("initgroups failed: %s", pas->pw_name);

    if (setgid(pas->pw_gid) < 0) 
	die("setgid failed: %s %d", pas->pw_name, pas->pw_gid);
    
    if (setuid(pas->pw_uid) < 0) 
	die("setuid failed: %s %d", pas->pw_name, pas->pw_uid);

    return(pas->pw_uid);
}


void
sig_dfl(void)
    /* set signals handling to its default */
{
	signal(SIGTERM, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGUSR1, SIG_DFL);
	signal(SIGUSR2, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
}


FILE *
create_mail(cl_t *line, char *subject) 
    /* create a temp file and write in it a mail header */
{
    /* create temporary file for stdout and stderr of the job */
    int mailfd = temp_file(NULL);
    FILE *mailf = fdopen(mailfd, "r+");
    char hostname[USER_NAME_LEN];
    /* is this a complete mail address ? (ie. with a "@", not only a username) */
    char complete_adr = 0;
    int i;

    if ( mailf == NULL )
	die_e("Could not fdopen() mailfd");

    /* write mail header */
    fprintf(mailf, "To: %s", line->cl_mailto);

#ifdef HAVE_GETHOSTNAME
    if (gethostname(hostname, sizeof(hostname)) != 0) {
	error_e("Could not get hostname");
	hostname[0] = '\0';
    }
    else {
	/* it is unspecified whether a truncated hostname is NUL-terminated */
	hostname[USER_NAME_LEN-1] = '\0';

	/* check if mailto is a complete mail address */
	for ( i = 0 ; line->cl_mailto[i] != '\0' ; i++ ) {
	    if ( line->cl_mailto[i] == '@' ) {
		complete_adr = 1;
		break;
	    }
	}
	if ( ! complete_adr )
	    fprintf(mailf, "@%s", hostname);
    }
#else
    hostname[0] = '\0';
#endif /* HAVE_GETHOSTNAME */

    if (subject)
	fprintf(mailf, "\nSubject: fcron <%s@%s> %s: %s\n\n", line->cl_file->cf_user,
		( hostname[0] != '\0')? hostname:"?" , subject, line->cl_shell);
    else
	fprintf(mailf, "\nSubject: fcron <%s@%s> %s\n\n", line->cl_file->cf_user,
		( hostname[0] != '\0')? hostname:"?" , line->cl_shell);


    return mailf;
}


int
read_write_pipe(int fd, void *buf, size_t size, int action)
    /* Read/write data from/to pipe.
     * action can either be PIPE_WRITE or PIPE_READ.
     * Handles signal interruptions, and read in several passes.
     * Returns ERR in case of a closed pipe, the errno from errno
     * for other errors, and OK if everything was read successfully */
{
    int size_processed = 0;
    int ret;
    
    while ( size_processed < size ) {
	if ( action == PIPE_READ )
	    ret = read(fd, (char *)buf + size_processed, size); 
	else if ( action == PIPE_WRITE )
	    ret = write(fd, (char *)buf + size_processed, size);
	else {
	    error("Invalid action parameter for function read_write_pipe():"
		  " %d", action);
	    return ERR;
	}
	if ( ret > 0 )
	    /* some data read correctly -- we still may need
	     * one or several calls of read() to read the rest */
	    size_processed += ret;
	else if ( ret < 0 && errno == EINTR )
	    /* interrupted by a signal : let's try again */
	    continue;
	else {
	    /* error */

	    if ( ret == 0 )
		/* is it really an error when writing ? should we continue
		 * in this case ? */
		return ERR;
	    else
		return errno;
	}
    }

    return OK;
}

int
read_pipe(int fd, void *buf, size_t size)
    /* Read data from pipe. 
     * Handles signal interruptions, and read in several passes.
     * Returns ERR in case of a closed pipe, the errno from errno
     * for other errors, and OK if everything was read successfully */
{
    return read_write_pipe(fd, buf, size, PIPE_READ);
}

int
write_pipe(int fd, void *buf, size_t size)
    /* Read data from pipe. 
     * Handles signal interruptions, and read in several passes.
     * Returns ERR in case of a closed pipe, the errno from errno
     * for other errors, and OK if everything was read successfully */
{
    return read_write_pipe(fd, buf, size, PIPE_WRITE);
}

void
run_job_grand_child_setup_stderr_stdout(cl_t *line, int *pipe_fd)
    /* setup stderr and stdout correctly so as the mail containing
     * the output of the job can be send at the end of the job.
     * Close the pipe (both ways). */
{

    if (is_mail(line->cl_option) ) {
	/* we can't dup2 directly to mailfd, since a "cmd > /dev/stderr" in
	 * a script would erase all previously collected message */
	if ( dup2( pipe_fd[1], 1) != 1 || dup2(1, 2) != 2 )
	    die_e("dup2() error");  /* dup2 also clears close-on-exec flag */
	/* we close the pipe_fd[]s : the resources remain, and the pipe will
	 * be effectively close when the job stops */
	close(pipe_fd[0]);
	close(pipe_fd[1]);
	/* Standard buffering results in unwanted behavior (some messages,
	   at least error from fcron process itself, are lost) */
#ifdef HAVE_SETLINEBUF
	setlinebuf(stdout);
	setlinebuf(stderr);
#else
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
#endif
    }
    else if ( foreground ) {
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
    }
    
}

void
run_job_grand_child_setup_nice(cl_t *line)
    /* set the nice value for the job */
{
    if ( line->cl_nice != 0 ) {
	errno = 0; /* so that it works with any libc and kernel */
	if ( nice(line->cl_nice) == -1  &&  errno != 0 )
	    error_e("could not set nice value");
    }
}


void
run_job_grand_child_setup_env_var(cl_t *line, char **curshell)
/* set the env var from the fcrontab, change dir to HOME and check that SHELL is ok 
 * Return the final value of SHELL in curshell. */
{
    env_t *env;
    char *home;

    for ( env = line->cl_file->cf_env_base; env; env = env->e_next)
	if ( putenv(env->e_val) != 0 )
	    error("could not putenv()");

    /* change dir to HOME */
    if ( (home = getenv("HOME")) != NULL )
	if (chdir(home) != 0) {
	    error_e("Could not chdir to HOME dir \"%s\"", home);
	    if (chdir("/") < 0)
		die_e("Could not chdir to HOME dir /");
	}

    /* check that SHELL is valid */
    if ( (*curshell = getenv("SHELL")) == NULL )
	*curshell = shell;
    else if ( access(*curshell, X_OK) != 0 ) {
	if (errno == ENOENT)
	    error("shell \"%s\" : no file or directory. SHELL set to %s",
		  *curshell, shell);
	else
	    error_e("shell \"%s\" not valid : SHELL set to %s",
		    *curshell, shell);
	*curshell = shell;
    }
}

void 
run_job(struct exe_t *exeent)
    /* fork(), redirect outputs to a temp file, and execl() the task */ 
{

    pid_t pid;
    cl_t *line = exeent->e_line;
    int pipe_pid_fd[2];
    int ret = 0;

    /* prepare the job execution */
    if ( pipe(pipe_pid_fd) != 0 ) {
	error_e("pipe(pipe_pid_fd) : setting job_pid to -1");
	exeent->e_job_pid = -1;
    }

#ifdef CHECKRUNJOB
    debug("run_job(): first pipe created successfully : about to do first fork()");
#endif /* CHECKRUNJOB */

    switch ( pid = fork() ) {
    case -1:
	error_e("Fork error : could not exec \"%s\"", line->cl_shell);
	break;

    case 0:
	/* child */
    {
	char *curshell;
	FILE *mailf = NULL;
	int status = 0;
 	int to_stdout = foreground && is_stdout(line->cl_option);
	int pipe_fd[2];
	short int mailpos = 0;	/* 'empty mail file' size */
#ifdef WITH_SELINUX
	int flask_enabled = is_selinux_enabled();
#endif
 
	/* // */
 	debug("run_job(): child: %s, output to %s, %s, %s\n",
	      is_mail(line->cl_option) || is_mailzerolength(line->cl_option) ?
	      "mail" : "no mail",
	      to_stdout ? "stdout" : "file",
 	      foreground ? "running in foreground" : "running in background",
 	      is_stdout(line->cl_option) ? "stdout" : "normal" );
	/* // */

	if ( ! to_stdout && 
	     ( is_mail(line->cl_option) || is_mailzerolength(line->cl_option))){
	    /* we create the temp file (if needed) before change_user(),
	     * as temp_file() needs the root privileges */
	    /* if we run in foreground, stdout and stderr point to the console.
	     * Otherwise, stdout and stderr point to /dev/null . */
	    mailf = create_mail(line, NULL);
	    mailpos = ftell(mailf);
	    if (pipe(pipe_fd) != 0) 
		die_e("could not pipe()");
	}

	/* First, restore umask to default */
	umask (saved_umask);

#ifndef RUN_NON_PRIVILEGED
	if (change_user(line) < 0)
	    return ;
#endif

	sig_dfl();

#ifdef CHECKRUNJOB
	debug("run_job(): child: change_user() done -- about to do 2nd fork()");
#endif /* CHECKRUNJOB */

	/* now, run the job */
	switch ( pid = fork() ) {
	case -1:
	    error_e("Fork error : could not exec \"%s\"", line->cl_shell);
	    if ( write(pipe_pid_fd[1], &pid, sizeof(pid)) < 0 )
		error_e("could not write child pid to pipe_pid_fd[1]");
	    close(pipe_fd[1]);
	    close(pipe_pid_fd[0]);
	    close(pipe_pid_fd[1]);
	    exit(EXIT_ERR);
	    break;

	case 0:
	    /* grand child (child of the 2nd fork) */
	    
	    if ( ! to_stdout )
		/* note : the following closes the pipe */
		run_job_grand_child_setup_stderr_stdout(line, pipe_fd);

	    foreground = 1; 
	    /* now, errors will be mailed to the user (or to /dev/null) */

	    run_job_grand_child_setup_nice(line);

	    xcloselog();

	    /* set env variables */
	    run_job_grand_child_setup_env_var(line, &curshell);

#if defined(CHECKJOBS) || defined(CHECKRUNJOB)
	    /* this will force to mail a message containing at least the exact
	     * and complete command executed for each execution of all jobs */
	    debug("run_job(): grand-child: Executing \"%s -c %s\"", curshell, line->cl_shell);
#endif /* CHECKJOBS OR CHECKRUNJOB */

#ifdef WITH_SELINUX
	    if(flask_enabled && setexeccon(line->cl_file->cf_user_context) )
		die_e("Can't set execute context \"%s\".",
		      line->cl_file->cf_user_context);
#else
	    if (setsid() == -1) {
		die_e("setsid(): errno %d", errno);
	    }
#endif
	    execl(curshell, curshell, "-c", line->cl_shell, NULL);
	    /* execl returns only on error */
	    error_e("Can't find \"%s\". Trying a execlp(\"sh\",...)",curshell);
	    execlp("sh", "sh",  "-c", line->cl_shell, NULL);
	    die_e("execl() \"%s -c %s\" error", curshell, line->cl_shell);

	    /* execution never gets here */

	default:
	    /* child (parent of the 2nd fork) */

	    /* close unneeded WRITE pipe and READ pipe */
	    close(pipe_fd[1]);
	    close(pipe_pid_fd[0]);

#ifdef CHECKRUNJOB
	    debug("run_job(): child: pipe_fd[1] and pipe_pid_fd[0] closed"
		  " -- about to write grand-child pid to pipe");
#endif /* CHECKRUNJOB */

	    /* give the pid of the child to the parent (main) fcron process */
	    ret = write_pipe(pipe_pid_fd[1], &pid, sizeof(pid));
	    if ( ret != OK ) {
		if ( ret == ERR )
		    error("run_job(): child: Could not write job pid"
			  " to pipe");
		else {
		    errno = ret;
		    error_e("run_job(): child: Could not write job pid"
			    " to pipe");
		}
		
		exeent->e_job_pid = -1;
		break;
	    }
	    
#ifdef CHECKRUNJOB
	    debug("run_job(): child: grand-child pid written to pipe");
#endif /* CHECKRUNJOB */

	    if ( ! is_nolog(line->cl_option) )
		explain("Job %s started for user %s (pid %d)", line->cl_shell,
			line->cl_file->cf_user, pid);

	    if ( ! to_stdout && is_mail(line->cl_option ) ) {
		/* user wants a mail : we use the pipe */
		char mailbuf[TERM_LEN];
		FILE *pipef = fdopen(pipe_fd[0], "r");

		if ( pipef == NULL )
		    die_e("Could not fdopen() pipe_fd[0]");

		mailbuf[sizeof(mailbuf)-1] = '\0';
		while ( fgets(mailbuf, sizeof(mailbuf), pipef) != NULL )
		    if ( fputs(mailbuf, mailf) < 0 )
			warn("fputs() failed to write to mail file for job %s (pid %d)",
			     line->cl_shell, pid);
		fclose(pipef); /* (closes also pipe_fd[0]) */
	    }

	    /* FIXME : FOLLOWING HACK USELESS ? */
	    /* FIXME : HACK
	     * this is a try to fix the bug on sorcerer linux (no jobs
	     * exectued at all, and 
	     * "Could not read job pid : setting it to -1: No child processes"
	     * error messages) */
	    /* use a select() or similar to know when parent has read
	     * the pid (with a timeout !) */
	    /* // */
	    sleep(2);
	    /* // */
#ifdef CHECKRUNJOB
	    debug("run_job(): child: closing pipe with parent");
#endif /* CHECKRUNJOB */
	    close(pipe_pid_fd[1]);

	    /* we use a while because of a possible interruption by a signal */
	    while ( (pid = wait3(&status, 0, NULL)) > 0)
		{
#ifdef CHECKRUNJOB
		    debug("run_job(): child: ending job pid %d", pid);
#endif /* CHECKRUNJOB */
		    end_job(line, status, mailf, mailpos);
		}

	    /* execution never gets here */
	    
	}

	/* execution never gets here */
    }

    default:
	/* parent */

	/* close unneeded WRITE fd */
	close(pipe_pid_fd[1]);

	exeent->e_ctrl_pid = pid;
	line->cl_file->cf_running += 1;

#ifdef CHECKRUNJOB
	debug("run_job(): about to read grand-child pid...");
#endif /* CHECKRUNJOB */

	/* read the pid of the job */
	ret = read_pipe(pipe_pid_fd[0], &(exeent->e_job_pid), sizeof(pid_t));
	if ( ret != OK ) {
	    if ( ret == ERR )
		error("Could not read job pid because of closed pipe:"
		      " setting it to -1");
	    else {
		errno = ret;
		error_e("Could not read job pid : setting it to -1");
	    }
	    
	    exeent->e_job_pid = -1;
	    break;
	}
	close(pipe_pid_fd[0]);
    }

#ifdef CHECKRUNJOB
    debug("run_job(): finished reading pid of the job -- end of run_job().");
#endif /* CHECKRUNJOB */

}

void 
end_job(cl_t *line, int status, FILE *mailf, short mailpos)
    /* if task have made some output, mail it to user */
{

    char mail_output;
    char *m;

    if ( mailf != NULL &&
	 (is_mailzerolength(line->cl_option) || 
	  ( ( is_mail(line->cl_option) &&
	      ( (fseek(mailf, 0, SEEK_END) == 0 && ftell(mailf) > mailpos) ||
		! (WIFEXITED(status) && WEXITSTATUS(status) == 0) ) ) ) ) )
	/* an output exit : we will mail it */
	mail_output = 1;
    else
	/* no output */
	mail_output = 0;

    m = (mail_output == 1) ? " (mailing output)" : "";
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
	if ( ! is_nolog(line->cl_option) )
	    explain("Job %s completed%s", line->cl_shell, m);
    }
    else if (WIFEXITED(status)) {
	warn("Job %s terminated (exit status: %d)%s",
	     line->cl_shell, WEXITSTATUS(status), m);
	/* there was an error : in order to inform the user by mail, we need
	 * to add some data to mailf */
	if ( mailf != NULL )
	    fprintf(mailf, "Job %s terminated (exit status: %d)%s",
		    line->cl_shell, WEXITSTATUS(status), m);
    }
    else if (WIFSIGNALED(status)) {
	error("Job %s terminated due to signal %d%s",
	      line->cl_shell, WTERMSIG(status), m);
	if ( mailf != NULL )
	    fprintf(mailf, "Job %s terminated due to signal %d%s",
		    line->cl_shell, WTERMSIG(status), m);
    }
    else { /* is this possible? */
	error("Job %s terminated abnormally %s", line->cl_shell, m);
	if ( mailf != NULL )
	    fprintf(mailf, "Job %s terminated abnormally %s", line->cl_shell, m);
    }

#ifdef HAVE_LIBPAM
    /* we close the PAM session before running the mailer command :
     * it avoids a fork(), and we use PAM anyway to control whether a user command
     * should be run or not.
     * We consider that the administrator can use a PAM compliant mailer to control
     * whether a mail can be sent or not.
     * It should be ok like that, otherwise contact me ... -tg */

    /* Aiee! we may need to be root to do this properly under Linux.  Let's
       hope we're more l33t than PAM and try it as non-root. If someone
       complains, I'll fix this :P -hmh */
    pam_setcred(pamh, PAM_DELETE_CRED | PAM_SILENT);
    pam_end(pamh, pam_close_session(pamh, PAM_SILENT));
#endif

    if (mail_output == 1) {
	launch_mailer(line, mailf);
	/* never reached */
	die_e("Internal error: launch_mailer returned");
    }

    /* if mail is sent, execution doesn't get here : close /dev/null */
    if ( mailf != NULL && fclose(mailf) != 0 )
	die_e("Can't close file mailf");

    exit(0);

}

void
launch_mailer(cl_t *line, FILE *mailf)
    /* mail the output of a job to user */
{
#ifdef SENDMAIL
    foreground = 0;

    /* set stdin to the job's output */

    /* fseek() should work, but it seems that it is not always the case
     * (users have reported problems on gentoo and LFS).
     * For those users, lseek() works, so I have decided to use both,
     * as I am not sure that lseek(fileno(...)...) will work as expected
     * on non linux systems. */
    if ( fseek(mailf, 0, SEEK_SET ) != 0) die_e("Can't fseek()");
    if ( lseek(fileno(mailf), 0, SEEK_SET ) != 0) die_e("Can't lseek()");
    if ( dup2(fileno(mailf), 0) != 0 ) die_e("Can't dup2(fileno(mailf))");

    xcloselog();

    if ( chdir("/") < 0 )
	die_e("Could not chdir to /");

    /* run sendmail with mail file as standard input */
    execl(sendmail, sendmail, SENDMAIL_ARGS, line->cl_mailto, NULL);
    error_e("Can't find \"%s\". Trying a execlp(\"sendmail\")", sendmail);
    execlp("sendmail", "sendmail", SENDMAIL_ARGS, line->cl_mailto, NULL);
    die_e("Can't exec " SENDMAIL);
#else /* defined(SENDMAIL) */
    exit(EXIT_OK);
#endif
}


