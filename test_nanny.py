import ctypes
import os
import signal

from nanny import Nanny, NannyChild, TIMEVAL
from tempfile import NamedTemporaryFile


def test_nanny_dynamic_calling():
    """ Test the __getattribute__ support in the Nanny object """

    n = Nanny()

    assert n.nanny_child_new("/bin/true")


def test_nanny_create_child():
    """ Test Nanny child creation """
    n = Nanny()
    n.globals.nanny_pid = os.getpid()
    global running
    running = True

    def handler(signum, frame):
        print 'Signal handler called with signal', signum
        global running
        running = False


    signal.signal(signal.SIGHUP, handler)
    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGQUIT, handler)
    signal.signal(signal.SIGABRT, handler)
    signal.signal(signal.SIGTERM, handler)


    n = Nanny()

    tf = NamedTemporaryFile()
    child = n.create_child("/bin/echo foo > %s" % tf.name)
    assert len(n.children) == 1
    tv = TIMEVAL()

    i = 0
    while running:
        n.nanny_oversee_children()
        n.nanny_timer_next(ctypes.byref(tv), None)
        n.nanny_select(ctypes.byref(tv))
        i += 1
        if i > 3:
            break
    tf.seek(0)
    dat = tf.read()
    tf.close()
    assert dat == "foo\n"



