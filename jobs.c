#include "shell.h"

typedef struct proc {
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job {
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */
#ifdef STUDENT
  (void)status;
  (void)pid;
  // wait for any child process, flags:
  // WNOHANG -> nonblocking
  // WUNTRACED (e.g. cat &) -> stopped
  // WCONTINOUED -> child resumed by SIGCONT
  while ((pid = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED | WCONTINUED)) >
         0) {
    for (int i = 0; i < njobmax; i++) { // for each job
      if (jobs[i].pgid == 0)            // if slot is free
        continue;

      bool has_running_p = false; // flags for all processes in a job
      bool has_stopped_p = false;

      for (int j = 0; j < jobs[i].nproc; j++) { // for each process
        proc_t *curr_proc = &jobs[i].proc[j];

        if (curr_proc->state == FINISHED)
          continue;

        // update the state and exitcode for pid returned by waitpid
        if (pid == curr_proc->pid) {
          curr_proc->exitcode = -1;

          if (WIFEXITED(status) || WIFSIGNALED(status)) {
            curr_proc->state = FINISHED;
            curr_proc->exitcode = status;
          } else if (WIFCONTINUED(status)) {
            curr_proc->state = RUNNING;
          } else if (WIFSTOPPED(status)) {
            curr_proc->state = STOPPED;
          } else
            fprintf(stderr, "wrong proc status\n");
        }

        // update flags for current jobs
        if (curr_proc->state == RUNNING)
          has_running_p = true;
        else if (curr_proc->state == STOPPED)
          has_stopped_p = true;
      }

      // update state of the job
      // if job has 1 one running process -> job is running
      // if job has 1 one stopped process -> job is stopped
      // else the job has finished
      if (has_running_p)
        jobs[i].state = RUNNING;
      else if (has_stopped_p)
        jobs[i].state = STOPPED;
      else
        jobs[i].state = FINISHED;
    }
  }
#endif /* !STUDENT */
  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job) {
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void) {
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
static int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
#ifdef STUDENT
  (void)exitcode;
  if (state == FINISHED) {
    *statusp = exitcode(job);
    deljob(job);
  }
#endif /* !STUDENT */

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

    /* TODO: Continue stopped job. Possibly move job to foreground slot. */
#ifdef STUDENT
  (void)movejob;

  jobs[j].state = RUNNING; // update state of the job explicitly

  if (bg == FG) {
    // slot should be free to request move to foreground
    assert(jobs[FG].pgid == 0);
    // take control of the terminal
    setfgpgrp(jobs[j].pgid);
    Tcsetattr(tty_fd, 0, &shell_tmodes);

    movejob(j, FG);
    Kill(-jobs[FG].pgid, SIGCONT); // "resume" job by signal

    msg("[%d] continue '%s'\n", j, jobcmd(FG));

    monitorjob(mask); // monitor job in foreground
  } else {
    Kill(-jobs[j].pgid, SIGCONT);
    msg("[%d] continue '%s'\n", j, jobcmd(j));
  }
#endif /* !STUDENT */

  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
#ifdef STUDENT
  Kill(-(jobs[j].pgid), SIGTERM);
  if (jobs[j].state == STOPPED) // stopped some of the race conditions
    Kill(-(jobs[j].pgid), SIGCONT);
#endif /* !STUDENT */

  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    if (jobs[j].pgid == 0)
      continue;

      /* TODO: Report job number, state, command and exit code or signal. */
#ifdef STUDENT
    (void)deljob;

    int exitcode = 0;
    // save before jobstate (which deletes on finishing)
    char *cmd = malloc(MAXLINE * sizeof(char));
    strcpy(cmd, jobcmd(j));

    int state = jobstate(
      j, &exitcode); // finished clean-up inside (instead of using deljob)

    // report on the state of a given job
    if ((which == ALL) || (which == state)) {
      if (state == FINISHED) {
        if (WIFEXITED(exitcode))
          msg("[%d] exited '%s', status=%d\n", j, cmd, WEXITSTATUS(exitcode));
        else
          msg("[%d] killed '%s' by signal %d\n", j, cmd, WTERMSIG(exitcode));
      } else if (state == RUNNING)
        msg("[%d] running '%s'\n", j, cmd);
      else if (state == STOPPED)
        msg("[%d] suspended '%s'\n", j, cmd);
    }

    free(cmd);

#endif /* !STUDENT */
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask) {
  int exitcode = 0, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
#ifdef STUDENT
  (void)jobstate;
  (void)exitcode;
  (void)state;
  // take control of terminal
  setfgpgrp(jobs[FG].pgid);
  while (true) {
    state = jobstate(FG, &exitcode);

    if (state == FINISHED || state == STOPPED) {
      break;
    }

    Sigsuspend(mask);
  }

  if (state == STOPPED) {
    // if stopped move to background
    int new_job = allocjob();
    movejob(FG, new_job);
  }

  // move shell to foreground
  setfgpgrp(getpgrp());

#endif /* !STUDENT */

  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void) {
  struct sigaction act = {
    .sa_flags = SA_RESTART,
    .sa_handler = sigchld_handler,
  };

  /* Block SIGINT for the duration of `sigchld_handler`
   * in case `sigint_handler` does something crazy like `longjmp`. */
  sigemptyset(&act.sa_mask);
  sigaddset(&act.sa_mask, SIGINT);
  Sigaction(SIGCHLD, &act, NULL);

  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void) {
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */
#ifdef STUDENT
  for (int idx_job = 0; idx_job < njobmax; idx_job++) {
    job_t *current_job = &jobs[idx_job];

    if (current_job->pgid == 0) // slot is free
      continue;

    if (current_job->state == FINISHED) // job has already finished
      continue;

    // pass job to fg
    if (idx_job != FG)
      setfgpgrp(current_job->pgid);

    // send kill signal
    killjob(idx_job);

    // wait for the job to finish
    while (current_job->state != FINISHED)
      Sigsuspend(&mask);

    // return terminal control to shell
    if (idx_job != FG)
      setfgpgrp(getpgrp());
  }
#endif /* !STUDENT */

  watchjobs(FINISHED);

  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}

/* Sets foreground process group to `pgid`. */
void setfgpgrp(pid_t pgid) {
  Tcsetpgrp(tty_fd, pgid);
}
