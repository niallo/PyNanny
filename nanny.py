import argparse

from ctypes import *


NANNY_TIMER_CB = CFUNCTYPE(c_void_p, c_long)

NANNY_CHILD_STATE_HANDLER = CFUNCTYPE(c_void_p, c_long)

class NANNY_TIMER(Structure):
    _fields_ = [("when", c_long),
                ("data", c_void_p),
                ("f", POINTER(NANNY_TIMER_CB))
               ]

class NANNY_TIMED_T(Structure):
    pass

# See http://docs.python.org/library/ctypes.html#incomplete-types
NANNY_TIMED_T._fields_ = [("next", POINTER(NANNY_TIMED_T)),
                ("timer", POINTER(NANNY_TIMER)),
                ("interval", c_long),
                ("last", c_long),
                ("cmd", c_char_p),
                ("envplen", c_int),
                ("envp", POINTER(c_char_p))
                ]


class NANNY_CHILD(Structure):
    """ Python object wrapper for C struct nanny_child (nanny/nanny.h)
    From the C header definition:

         Information about a child process.  This structure handles both
         "main" child processes and some auxiliary processes (such as health
         checks, which we need to time out).  Other auxiliary processes
         (such as stop scripts) just get started and abandoned and so don't
         get an entry here.  Maybe that should change....
    """
    pass

NANNY_CHILD_ENDED = CFUNCTYPE(POINTER(NANNY_CHILD), c_int, c_void_p)

NANNY_CHILD._fields_ = [("older", POINTER(NANNY_CHILD)),
                ("younger", POINTER(NANNY_CHILD)),
                ("instance", c_char_p),
                ("start_cmd", c_char_p),
                ("stop_cmd", c_char_p),
                ("health_cmd", c_char_p),
                ("restartable", c_bool),
                ("id", c_int),
                ("pid", c_int),
                ("running", c_int),
                ("last_start", c_long),
                ("last_stop", c_long),
                ("start_count", c_int),
                ("failures", c_int),
                ("restart_delay", c_int),
                ("ended", POINTER(NANNY_CHILD_ENDED)),
                ("state_handler", POINTER(NANNY_CHILD_STATE_HANDLER)),
                ("state_timer", POINTER(NANNY_TIMER)),
                ("state", c_char_p),
                ("timed", POINTER(NANNY_TIMED_T)),
                ("main", POINTER(NANNY_CHILD)),
                ("health_timer", POINTER(NANNY_TIMER)),
                ("health_failures_consecutive", c_int),
                ("health_failures_total", c_int),
                ("health_successes_consecutive", c_int),
                ("health_successes_total", c_int),
                ("child_stderr", POINTER(NANNY_LOG)),
                ("child_stdout", POINTER(NANNY_LOG)),
                ("child_events", POINTER(NANNY_LOG)),
                ("envp", POINTER(c_char_p))
                ]


class NANNY_LOG(Structure):

    _fields_ = [("refcnt", c_int),
                ("filename_base", c_char_p),
                ("filname", c_char_p),
                ("file_fd", c_int),
                ("last_rotate", c_long),
                ("last_rotate_bytes", c_ulonglong),
                ("last_rotate_check", c_long),
                ("total_bytes", c_ulonglong),
                ("read_count", c_ulonglong),
                ("error_count", c_ulonglong),
                ("bytes_per_second", c_float),
                ("bps_last_update_time", c_long),
                ("buff", c_char_p),
                ("buff_size", c_ulong),
                ("buff_end", c_char_p),
                ("buffp", c_char_p)]

class Nanny(object):
    def __init__(self):
        pass

class NannyChild(object):
    def __init__(self):
        pass

def main():
    parser = argparse.ArgumentParser(
        description='Nanny Demonstration'
        )

    args = parser.parse_args()


if __name__ == "__main__":
    main()
