// wrap a child process which consumes and creates datums via stdin/stdout
// write datums to child, one by one, via a pipe
// read datums from child, one by one, via a pipe

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "signal.h"
#include "poll.h"
#include "phish.h"

#define MAXLINE 1024

void writepipe(int);
void readpipe();
void close();
void sig_pipe(int);

/* ---------------------------------------------------------------------- */

pid_t pid;                        // child PID
int fd1[2],fd2[2];                // duplex pipes between parent/child
struct pollfd fdarray[1];         // polling data structure
int doneflag = 0;                 // set to 1 when child exits
char buf[MAXLINE];
int nbuf = 0;

/* ---------------------------------------------------------------------- */

int main(int narg, char **args)
{
  phish_init(&narg,&args);
  phish_input(0,writepipe,close,1);
  phish_output(0);
  phish_check();

  if (narg < 1) phish_error("Wrapss syntax: wrapss program");

  // combine all args into one string to launch with popen()
  // would be better if there was exactly one arg
  // but mpiexec strips quotes from quoted args

  char program[1024] = "";
  for (int i = 0; i < narg; i++) {
    strcat(program,args[i]);
    if (i < narg-1) strcat(program," ");
  }

  // set up 2 pipes between parent/child

  if (signal(SIGPIPE,sig_pipe) == SIG_ERR) {
    printf("signal error\n");
    exit(1);
  }

  if (pipe(fd1) < 0 || pipe(fd2) < 0) {
    printf("pipe error\n");
    exit(1);
  }

  // fork into parent & child

  pid = fork();
  if (pid < 0) {
    printf("fork error\n");
    exit(1);
  }

  // parent process
  // setup polling on readpipe from child
  // phish_probe is endless loop on incoming messages and child datums

  if (pid > 0) {
    close(fd1[0]);
    close(fd2[1]);

    fdarray[0].fd = fd2[0];
    fdarray[0].events = POLLIN;

    phish_probe(readpipe);
    phish_exit();

  // child process
  // map my stdin/stdout to pipes
  // exec() the child program

  } else {
    close(fd1[1]);
    close(fd2[0]);

    if (fd1[0] != STDIN_FILENO) {
      if (dup2(fd1[0],STDIN_FILENO) != STDIN_FILENO) {
	printf("dup2 error to stdin\n");
	close(fd1[0]);
	exit(1);
      }
    }
    if (fd2[1] != STDOUT_FILENO) {
      if (dup2(fd2[1],STDOUT_FILENO) != STDOUT_FILENO) {
	printf("dup2 error to stdout\n");
	close(fd2[1]);
	exit(1);
      }
    }

    if (execv(program,NULL) < 0) {
      printf("execv error\n");
      exit(1);
    }
  }
}

/* ----------------------------------------------------------------------
   write a line to the child process
------------------------------------------------------------------------- */

void writepipe(int nvalues)
{
  char *buf;
  int len;

  if (nvalues != 1) phish_error("Wrapss processes one-value datums");
  int type = phish_unpack(&buf,&len);
  if (type != PHISH_STRING) phish_error("Wrapss processes string values");

  int n1 = write(fd1[1],buf,len);
  int n2 = write(fd1[1],"\n",2);
  if (n1 != len) {
    printf("write error\n");
    exit(1);
  }
}

/* ----------------------------------------------------------------------
   read one or more lines from the child process
------------------------------------------------------------------------- */

void readpipe()
{
  int nbytes;
  char *line;

  // poll for any data in pipe

  int flag = poll(fdarray,1,0);
  if (flag < 0) {
    printf("execv error\n");
    exit(1);
  }

  // if data in pipe, read whatever is there into buf
  // break into lines, phish_send() each line downstream
  // retain last chunk of buf if not terminated by a newline
  // NOTE: could poll again at end of loop and continue while pipe has data
  //       would be analagous to MPI probe/read in phish_probe()

  if (fdarray[0].revents) {
    int n = read(fd2[0],&buf[nbuf],MAXLINE-1-nbuf);
    if (n < 0) {
      printf("read error\n");
      exit(1);
    }

    // child exited and closed pipe, so set doneflag

    if (n == 0) {
      doneflag = 1;
      return;
    }

    nbuf += n;
    buf[nbuf] = '\0';
    n = 0;

    printf("WSS: %d %s\n",nbuf,buf);

    line = strtok(buf,"\n");
    while (line) {
      nbytes = strlen(line);
      if (nbytes == 0) break;
      else if (n + nbytes == nbuf) {
	strcpy(buf,line);
      } else {
	n += nbytes+1;
	printf("PACKSTR %d %s\n",strlen(line),line);
	phish_pack_string(line);
	phish_send(0);
      }
      line = strtok(NULL,"\n");
    }

    nbuf -= n;
  }
}

/* ---------------------------------------------------------------------- */

void close()
{
  // close write pipe to child, so child sees EOF
  // read from child pipe until child exits and closes pipe
  // close read pipe from child

  close(fd1[1]);
  while (!doneflag) readpipe();
  close(fd2[0]);
}

/* ---------------------------------------------------------------------- */

void sig_pipe(int signo)
{
  printf("SIGPIPE error\n");
  exit(1);
}
