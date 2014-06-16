#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <readline/readline.h>

#include "dchat_h/log.h"


#ifndef LOG_PRI
#define LOG_PRI(p) ((p) & LOG_PRIMASK)
#endif

//! FILE pointer to log
static FILE* log_ = NULL;
//! log level
static int level_ = LOG_DEBUG;
static const char* flty_[8] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};


static void __attribute__((constructor)) init_log0(void)
{
    log_ = stderr;
}


/*! Log a message to a file.
 *  @param out Open FILE pointer
 *  @param lf Logging priority (equal to syslog)
 *  @param fmt Format string
 *  @param ap Variable parameter list
 */
static void vlog_msgf(FILE* out, int lf, const char* fmt, va_list ap)
{
    int level = LOG_PRI(lf);
    char buf[1024];

    if(level_ < level)
    {
        return;
    }

    if(out != NULL)
    {
        fprintf(out, "[%7s] ", flty_[level]);
        vfprintf(out, fmt, ap);
        fprintf(out, "\n");
    }

    else
    {
        vsnprintf(buf, sizeof(buf), fmt, ap);
        syslog(level | LOG_DAEMON, "%s", buf);
    }
}


/*! Log a message. This function automatically determines
 *  to which streams the message is logged.
 *  @param lf Log priority.
 *  @param fmt Format string.
 *  @param ... arguments
 */
void log_msg(int lf, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_msgf(log_, lf, fmt, ap);
    va_end(ap);
}


void log_errno(int lf, const char* str)
{
    log_msg(lf, "%s: '%s'", str, strerror(errno));
}


/*! This function output len bytes starting at buf in hexadecimal numbers.
 *  */
void log_hex(int lf, const void* buf, int len)
{
    static const char hex[] = "0123456789abcdef";
    char tbuf[100];
    int i;

    for(i = 0; i < len; i++)
    {
        snprintf(tbuf + (i & 0xf) * 3, sizeof(tbuf) - (i & 0xf) * 3, "%c%c ",
                 hex[(((char*) buf)[i] >> 4) & 15], hex[((char*) buf)[i] & 15]);

        if((i & 0xf) == 0xf)
        {
            log_msg(lf, "%s", tbuf);
        }
    }

    if((i & 0xf))
    {
        log_msg(lf, "%s", tbuf);
    }
}
