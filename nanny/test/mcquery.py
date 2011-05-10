#!/usr/bin/env python
'''
  Copyright (c) 2009 Metaweb Technologies, Inc.  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
  3. Neither the name of Metaweb Technologies nor the names of its contributors
     may be used to endorse or promote products derived from this software
     without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDES AND CONTRIBUTORS ``AS IS''
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
'''

import asyncore
import os
import socket
import sys
import time
from random import randint

# Use our multicast group to query all machines in the local cluster
MCAST_ADDR = "226.1.1.1"
# Use 127.0.0.1 to just query the local machine
#MCAST_ADDR = "127.0.0.1"
MCAST_PORT = 8889

class receiver(asyncore.dispatcher):
    def __init__(self):
        asyncore.dispatcher.__init__(self)
        self.create_socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    def handle_connect(self):
        pass
    def writable(self):
        return False
    def handle_read(self):
        data,addr = self.recvfrom(2048)
        print "Message from %s:%d: %s" % (addr[0], addr[1], data)

def poll():
    c = receiver()
    c.sendto("?" + "\t".join(sys.argv[1:]), (MCAST_ADDR, MCAST_PORT))
    last = start = time.time()
    while True:
        asyncore.poll(10)
        now = time.time()
        if now > last + 5 and now > start + 30:
            sys.exit()
        last = now

try:
	poll()
except KeyboardInterrupt:
       pass
