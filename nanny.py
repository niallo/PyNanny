import argparse

from ctypes import *


NANNY_TIMER_CB = CFUNCTYPE(None, c_void_p, c_long)

NANNY_CHILD_STATE_HANDLER = CFUNCTYPE(None, c_void_p, c_long)

class TIMEVAL(Structure):
    _fields_ = [("tv_sec", c_uint),
                ("tv_usec", c_uint)
               ]

class NANNY_GLOBALS(Structure):
    _fields_ = [("now", c_long),
                ("http_port", c_int),
                ("udp_inicast_socket", c_int),
                ("udp_multicast_addr", c_void_p),
                ("sigchld_count", c_int),
                ("sigchld_handled", c_int),
                ("nanny_pid", c_int),
                ("child_pid", c_int)
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
                ("buffp", c_char_p)
                ]


class NANNY_TIMER(Structure):
    _fields_ = [("when", c_long),
                ("data", c_void_p),
                ("f", NANNY_TIMER_CB)
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

NANNY_CHILD_ENDED = CFUNCTYPE(None, POINTER(NANNY_CHILD), c_int, c_void_p)

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
                ("ended", NANNY_CHILD_ENDED),
                ("state_handler", NANNY_CHILD_STATE_HANDLER),
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

class NANNY_HTTP_CONNECTION(Structure):
    _fields_ = [("sock", c_int),
                ("keepalive", c_int),
                ("buff", c_char_p),
                ("buff_end", c_char_p),
                ("buff_size", c_uint),
                ("start", c_char_p),
                ("end", c_char_p)
               ]

class NANNY_HTTP_REQUEST(Structure):
    pass

NANNY_HTTP_HEADER_PROCESSOR = CFUNCTYPE(c_int, POINTER(NANNY_HTTP_REQUEST),
        c_char_p, c_char_p)
NANNY_HTTP_BODY_PROCESSOR = CFUNCTYPE(c_int, POINTER(NANNY_HTTP_REQUEST))


NANNY_HTTP_REQUEST._fields_ = [
            ("connection", POINTER(NANNY_HTTP_CONNECTION)),
            ("uri", c_char_p),
            ("method", c_int),
            ("method_name", c_char_p),
            ("HTTPmajor", c_int),
            ("HTTPminor", c_int),
            ("header_processor", NANNY_HTTP_HEADER_PROCESSOR),
            ("body_processor", NANNY_HTTP_BODY_PROCESSOR),
            ("data", c_void_p)
]

NANNY_HTTP_DISPATCHER = CFUNCTYPE(None, POINTER(NANNY_HTTP_REQUEST))


HTTP_METHOD_GET = 1
HTTP_METHOD_PUT = 2
HTTP_METHOD_POST = 3


class Nanny(object):
    nanny_so = CDLL("nanny/libnanny.so")


    def __init__(self):
        self.children = []
        ng = NANNY_GLOBALS
        self.globals = ng.in_dll(self.nanny_so, "nanny_globals")

    def __getattribute__(self, name):
        """ Method proxy. Dynamically support all nanny_foo* functions. """

        def proxy_method_name(name):
            # Check whether we have a Python method of that name
            try:
                m = super(Nanny, self).__getattribute__(name)
                return m
            except AttributeError:
                pass
            valid_prefixes = ("nanny", "udp", "http")
            for p in valid_prefixes:
                if name.startswith(p):
                    lib = super(Nanny, self).__getattribute__("nanny_so")
                    f = getattr(lib, name)
                    return f
            raise AttributeError()

        return proxy_method_name(name)

    def create_child(self, start):
        child_new = self.nanny_so.nanny_child_new
        child_new.restype = POINTER(NANNY_CHILD)

        child_struct = child_new(start)
        child = NannyChild(self.nanny_so, child_struct)
        self.children.append(child)
        return child

class NannyChild(object):

    def __init__(self, nanny_so, child_struct):
        self._nanny_so = nanny_so
        self._child_struct = child_struct
        self._as_parameter_ = child_struct

    def set_health(self, health_cmd):
        self._nanny_so.nanny_child_set_health(self._child_struct, health_cmd)

    def set_stop(self, stop_cmd):
        self._nanny_so.nanny_child_set_stop(self._child_struct, stop_cmd)

    def set_restartable(self, restartable):
        self._nanny_so.nanny_child_set_restartable(self._child_struct,
                restartable)

    def add_periodic(self, periodic):
        self._nanny_so.nanny_child_add_periodic(self._child_struct, periodic)

    def set_logpath(self, logpath):
        self._nanny_so.nanny_child_set_logpath(self._child_struct, logpath)

def main():

    parser = argparse.ArgumentParser(
        description='Nanny Demonstration'
        )

    args = parser.parse_args()


if __name__ == "__main__":
    main()
