
import os
import signal

from ctypes import *
from nanny import Nanny, NannyChild, TIMEVAL, NANNY_HTTP_DISPATCHER
from tempfile import NamedTemporaryFile


def test_nanny_dynamic_calling():
    """ Test the __getattribute__ support in the Nanny object """

    n = Nanny()

    assert n.nanny_child_new("/bin/true")


def test_nanny_create_child():
    """ Test Nanny child creation and execution of a process """
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
        n.nanny_timer_next(byref(tv), None)
        n.nanny_select(byref(tv))
        i += 1
        if i > 3:
            break
    tf.seek(0)
    dat = tf.read()
    tf.close()
    assert dat == "foo\n"

def http_dispatcher(request):
    print "foooooooooo"
    #request.body_processor = n.nanny_children_http_status

def test_nanny_webserver():
    """ Test the nanny webserver """
    n = Nanny()
    n.globals.nanny_pid = os.getpid()

    tv = TIMEVAL()


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

    child = n.create_child("/bin/true")
    http_server_init = n.http_server_init
    http_server_init.restype = None
    http_server_init(None, 0, NANNY_HTTP_DISPATCHER(http_dispatcher))
    print "HTTP_PORT=%d" % n.globals.http_port
    tv = TIMEVAL()

    while running:
        n.nanny_oversee_children()
        n.nanny_timer_next(byref(tv), None)
        n.nanny_select(byref(tv))
