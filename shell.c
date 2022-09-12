#ifdef READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static void sigint_handler(int sig) {
  /* No-op handler, we just need break read() call with EINTR. */
  (void)sig;
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
#ifdef STUDENT
    (void)mode;
    (void)MaybeClose;

    if (token[i] == T_INPUT) {
      mode = T_INPUT;
      MaybeClose(inputp);
      *inputp = Open(token[i + 1], O_RDONLY, 0);
      token[i] = T_NULL;
      token[++i] = T_NULL;
    } else if (token[i] == T_OUTPUT) {
      mode = T_OUTPUT;
      MaybeClose(outputp);
      *outputp = Open(token[i + 1], O_WRONLY | O_CREAT, S_IRWXU);
      token[i] = T_NULL;
      token[++i] = T_NULL;
    } else if (mode == NULL)
      n++;
    else
      n++;

#endif /* !STUDENT */
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
#ifdef STUDENT
  pid_t pid;

  if ((pid = Fork()) == 0) { /* child */
    // restore default signal handling
    Sigprocmask(SIG_SETMASK, &mask, NULL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGINT, SIG_DFL);
    if (bg) { // this if solved FD leaks
      // signals for terminal read/write for bg jobs
      Signal(SIGTTIN, SIG_DFL);
      Signal(SIGTTOU, SIG_DFL);
    }
    pid_t child_pid = getpid();
    // using setpgid instead of Setpgid because of permission errors
    setpgid(child_pid, child_pid);
    // pgid == pid

    if (bg == FG) {
      setfgpgrp(child_pid);
    }

    // redirect input/output
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      Close(input);
    }

    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      Close(output);
    }

    // handle buitling_commands in foreground
    int exitcode = -1;
    if ((exitcode = builtin_command(token)) >= 0) {
      exit(exitcode);
    }

    // search PATH and execve token
    external_command(token);
  } else if (pid > 0) { /* parent */
    pid_t pgid = pid;
    setpgid(pgid, pgid);

    // may be open by do_redir
    MaybeClose(&input);
    MaybeClose(&output);

    int job_id = addjob(pgid, bg);
    addproc(job_id, pgid, token); // execute for token

    if (bg)
      msg("[%d] running '%s'\n", job_id, jobcmd(job_id));
    else
      exitcode = monitorjob(&mask); // monitor foreground job

  } else { // wrong pid
    return -1;
  }
#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens, bool bg) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
#ifdef STUDENT
  if (pid == 0) { /* child */
    // restore default signal handling
    Sigprocmask(SIG_SETMASK, mask, NULL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGINT, SIG_DFL);
    if (bg) {
      Signal(SIGTTIN, SIG_DFL);
      Signal(SIGTTOU, SIG_DFL);
    }

    setpgid(0, pgid);

    // redirect input/output
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      Close(input);
    }

    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      Close(output);
    }

    int exitcode = -1;
    if ((exitcode = builtin_command(token)) >= 0) {
      exit(exitcode);
    }

    external_command(token);
  } else if (pid > 0) { /* parent */
    setpgid(pid, pgid);

    MaybeClose(&input);
    MaybeClose(&output);
  } else {
    return -1;
  }
#endif /* !STUDENT */

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
#ifdef STUDENT
  (void)input;
  (void)job;
  (void)pid;
  (void)pgid;
  (void)do_stage;

  // we have one pipeline job & multiple process
  // so divide processes by T_PIPE token
  // and treat the first and last process as special cases

  size_t processed_tokens = 0;
  bool is_last_token = false;

  while (processed_tokens < ntokens) {
    size_t i = processed_tokens;

    // find next pipe symbol
    while (i < ntokens) {
      if ((i == ntokens - 1) ||
          ((i == ntokens - 2) && token[ntokens - 1] == T_BGJOB)) {
        // last token (special case for '&' token at the nd)
        output = -1;
        is_last_token = true;
      }

      if (token[i] == T_PIPE) {
        token[i] = NULL;
        break;
      }

      i++;
    }

    // new processes' tokens start and length
    token_t *new_start = token + processed_tokens;
    size_t sub_len = i - processed_tokens;

    // update for next while-loop iteration
    processed_tokens = i + 1;

    // if it is not the first process -> redirect
    if (job != -1)
      input = next_input;

    // if it is neither first or last -> create pipe
    if (!is_last_token && job != -1) {
      mkpipe(&next_input, &output);
    }

    // middle and last process -> use pgid of job (first process)
    if (job != -1) {
      pid = do_stage(pgid, &mask, input, output, new_start, sub_len, bg);
      addproc(job, pid, new_start);
    }

    // first process -> define pgid and job id
    if (job == -1) {
      pgid = do_stage(pgid, &mask, input, output, new_start, sub_len, bg);
      job = addjob(pgid, bg);
      addproc(job, pgid, token);
    }
  }

  // monitor background job
  if (!bg)
    exitcode = monitorjob(&mask);
#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

#ifndef READLINE
static char *readline(const char *prompt) {
  static char line[MAXLINE]; /* `readline` is clearly not reentrant! */

  write(STDOUT_FILENO, prompt, strlen(prompt));

  line[0] = '\0';

  ssize_t nread = read(STDIN_FILENO, line, MAXLINE);
  if (nread < 0) {
    if (errno != EINTR)
      unix_error("Read error");
    msg("\n");
  } else if (nread == 0) {
    return NULL; /* EOF */
  } else {
    if (line[nread - 1] == '\n')
      line[nread - 1] = '\0';
  }

  return strdup(line);
}
#endif

int main(int argc, char *argv[]) {
  /* `stdin` should be attached to terminal running in canonical mode */
  if (!isatty(STDIN_FILENO))
    app_error("ERROR: Shell can run only in interactive mode!");

#ifdef READLINE
  rl_initialize();
#endif

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  struct sigaction act = {
    .sa_handler = sigint_handler,
    .sa_flags = 0, /* without SA_RESTART read() will return EINTR */
  };
  Sigaction(SIGINT, &act, NULL);

  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  while (true) {
    char *line = readline("# ");

    if (line == NULL)
      break;

    if (strlen(line)) {
#ifdef READLINE
      add_history(line);
#endif
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
