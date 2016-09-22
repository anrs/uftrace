% UFTRACE-DUMP(1) Uftrace User Manuals
% Namhyung Kim <namhyung@gmail.com>
% May, 2016

NAME
====
uftrace-dump - Print raw tracing data in the data files

SYNOPSIS
========
uftrace dump [*options*]

DESCRIPTION
===========
This command shows raw tracing data recorded in the data file.


OPTIONS
=======
\--debug
:   Show hex dump of data as well

\--chrome
:   Show JSON style output used by Google chrome tracing facility.

-k, \--kernel
:   Show dump of kernel functions as well as user functions.

-K, \--kernel-full
:   Show dump of kernel functions only.

-F *FUNC*, \--filter=*FUNC*
:   Set filter to dump selected functions only.  This option can be used more than once.  See `uftrace-replay` for filters.

-N *FUNC*, \--notrace=*FUNC*
:   Set filter not to dump selected functions (and their children).  This option can be used more than once.  See `uftrace-replay` for filters.

-T *TRG*, \--trigger=*TRG*
:   Set trigger on selected functions.  This option can be used more than once.  See `uftrace-replay` for triggers.

\--tid=*TID*[,*TID*,...]
:   Only dump functions from given threads.  To see the list of threads in the data file, you can use `uftrace-report --threads` or `uftrace-info` command.

-D *DEPTH*, \--depth *DEPTH*
:   Set trace limit in nesting level.


EXAMPLE
=======
This command dumps data like below:

    $ uftrace record abc

    $ uftrace dump
    ftrace file header: magic         = 4674726163652100
    ftrace file header: version       = 4
    ftrace file header: header size   = 40
    ftrace file header: endian        = 1 (little)
    ftrace file header: class         = 2 (64 bit)
    ftrace file header: features      = 0x3
    ftrace file header: info          = 0x3ff

    reading 23043.dat
    105430.415350255  23043: [entry] __monstartup(4004d0) depth: 0
    105430.415351178  23043: [exit ] __monstartup(4004d0) depth: 0
    105430.415351932  23043: [entry] __cxa_atexit(4004f0) depth: 0
    105430.415352687  23043: [exit ] __cxa_atexit(4004f0) depth: 0
    105430.415353833  23043: [entry] main(400512) depth: 0
    105430.415353992  23043: [entry] a(4006b2) depth: 1
    105430.415354112  23043: [entry] b(4006a0) depth: 2
    105430.415354230  23043: [entry] c(400686) depth: 3
    105430.415354425  23043: [entry] getpid(4004b0) depth: 4
    105430.415355035  23043: [exit ] getpid(4004b0) depth: 4
    105430.415355549  23043: [exit ] c(400686) depth: 3
    105430.415355761  23043: [exit ] b(4006a0) depth: 2
    105430.415355943  23043: [exit ] a(4006b2) depth: 1
    105430.415356109  23043: [exit ] main(400512) depth: 0

    $ uftrace dump --chrome
    {"traceEvents":[
    {"ts":105430415350,"ph":"B","pid":23043,"name":"__monstartup"},
    {"ts":105430415351,"ph":"E","pid":23043,"name":"__monstartup"},
    {"ts":105430415351,"ph":"B","pid":23043,"name":"__cxa_atexit"},
    {"ts":105430415352,"ph":"E","pid":23043,"name":"__cxa_atexit"},
    {"ts":105430415353,"ph":"B","pid":23043,"name":"main"},
    {"ts":105430415353,"ph":"B","pid":23043,"name":"a"},
    {"ts":105430415354,"ph":"B","pid":23043,"name":"b"},
    {"ts":105430415354,"ph":"B","pid":23043,"name":"c"},
    {"ts":105430415354,"ph":"B","pid":23043,"name":"getpid"},
    {"ts":105430415355,"ph":"E","pid":23043,"name":"getpid"},
    {"ts":105430415355,"ph":"E","pid":23043,"name":"c"},
    {"ts":105430415355,"ph":"E","pid":23043,"name":"b"},
    {"ts":105430415355,"ph":"E","pid":23043,"name":"a"},
    {"ts":105430415356,"ph":"E","pid":23043,"name":"main"}
    ], "metadata": {
    "command_line":"uftrace record abc ",
    "recorded_time":"Tue May 24 19:44:54 2016"
    } }


SEE ALSO
========
`uftrace`(1), `uftrace-record`(1)
