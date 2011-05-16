import urllib
import os
import signal
import time

from ctypes import *
from nanny import Nanny, NannyChild, TIMEVAL, NANNY_HTTP_DISPATCHER, NANNY_HTTP_BODY_PROCESSOR
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


def test_nanny_webserver():
    """ Test the nanny webserver """
    n = Nanny()
    n.globals.nanny_pid = os.getpid()

    tv = TIMEVAL()

    global running
    running = True

    def handler(signum, frame):
        global running
        running = False

    def http_default_page(request):
        http_printf = n.http_printf
        http_printf.restype = None
        nanny_hostname = n.nanny_hostname
        nanny_hostname.restype = c_char_p
        nanny_isotime = n.nanny_isotime
        nanny_isotime.restype = c_char_p

        http_printf(request, "HTTP/1.0 200 OK\x0d\x0a")
        http_printf(request, "Content-Type: text/html\x0d\x0a")
        http_printf(request, "\x0d\x0a")
        http_printf(request, "<HTML>\n");
        http_printf(request, "<head><title>Nanny: %s</title></head>\n",
              nanny_hostname())
        http_printf(request, "<body>\n")
        http_printf(request, "<ul>\n")
        http_printf(request, "<li>Host: %s\n", nanny_hostname())
        http_printf(request, "<li>Time: %s\n", nanny_isotime(0))
        http_printf(request, "<li><a href=\"/status/\">Children</a><br/>\n")
        http_printf(request, "<li><a href=\"/environment\">Environment</a><br/>\n")
        http_printf(request, "</ul>\n")
        http_printf(request, "</body>\n")
        http_printf(request, "</HTML>\n")

        return 0

    def http_dispatcher(request):
        if request.contents.uri.startswith("/status"):
            request.contents.body_processor = status_body_proc
            return
        if request.contents.uri.startswith("/environment"):
            request.contents.body_processor = env_body_proc
            return
        request.contents.body_processor = default_body_proc

    # Must keep reference around. Otherwise Python may GC it.
    status_body_proc = NANNY_HTTP_BODY_PROCESSOR(n.nanny_children_http_status)
    env_body_proc = NANNY_HTTP_BODY_PROCESSOR(n.nanny_http_environ_body)
    default_body_proc = NANNY_HTTP_BODY_PROCESSOR(http_default_page)
    dispatcher = NANNY_HTTP_DISPATCHER(http_dispatcher)

    signal.signal(signal.SIGHUP, handler)
    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGQUIT, handler)
    signal.signal(signal.SIGABRT, handler)
    signal.signal(signal.SIGTERM, handler)


    n = Nanny()

    child = n.create_child("/bin/true")
    http_server_init = n.http_server_init
    http_server_init.restype = None

    http_server_init(None, 0, dispatcher)
    childpid = os.fork()
    if childpid == 0:
        tv = TIMEVAL()

        while running:
            n.nanny_oversee_children()
            n.nanny_timer_next(byref(tv), None)
            n.nanny_select(byref(tv))
        # die hard once we get the signal from parent
        os._exit(0)
    else:
        # wait 2 seconds before trying to fetch from the webserver.
        time.sleep(2)
        f = urllib.urlopen("http://localhost:%d" % n.globals.http_port)
        dat = f.read()
        f.close()
        # clean up the child process
        os.kill(childpid, signal.SIGTERM)
        assert 'status' in dat
