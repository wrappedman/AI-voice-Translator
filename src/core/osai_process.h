#ifndef OSAI_PROCESS_H
#define OSAI_PROCESS_H
#include <stdbool.h>
#include <stddef.h>
#include "osai_common.h"
typedef struct osa_process osa_process;
/* argv[0] is an executable path, not a shell command. NULL terminates argv.
 * The child inherits no listening socket; only anonymous stdio pipes. */
osa_process *osa_process_start(const char *const *argv, osa_error *err);
/* Reads one complete JSON line. timeout_ms<=0 means no deadline. Returns false
 * on cancellation, timeout, EOF or oversized output. Caller frees nothing. */
bool osa_process_line(osa_process *p, int timeout_ms, volatile int *cancel,
                      char **line, osa_error *err);
bool osa_process_write(osa_process *p, const char *line, osa_error *err);
/* True when the child has already terminated; *code receives its exit status
 * (signal deaths report 128+signal). Used to explain a failed model load. */
bool osa_process_exited(osa_process *p, int *code);
void osa_process_stop(osa_process *p);
#endif
