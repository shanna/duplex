"""Megasock prototype

This is a functional prototype in Python for Megasock. In true form, it
would be written in C and have various performance tricks. The C library
would then be wrapped by various language bindings, making this high
performance socket joining library accessible in many languages.
"""
import threading
import select
import socket
import os

# Flags used in public API
HALFDUPLEX = 1
NOCLOSE = 2

class JoinStream(object):
    """One-way stream from one socket to another"""

    socket_from = None
    socket_to = None
    transform = None
    link_close = True

    def __init__(self, ctx, from_, to, transform=None, link_close=True):
        self.socket_from = ctx.managed_socket(from_)
        self.socket_from.streams_out.append(self)

        self.socket_to = ctx.managed_socket(to)
        self.socket_to.streams_in.append(self)

        self.transform = transform
        self.link_close = link_close

    def stop(self):
        self.socket_from.streams_out.remove(self)
        self.socket_from = None

        self.socket_to.streams_in.remove(self)
        self.socket_to = None

    def __repr__(self):
        return "<{}|{}>".format(self.socket_from, self.socket_to)



class ManagedSocket(object):
    socket = None
    streams_out = None
    streams_in = None
    write_buffer = None
    close_ready = False

    def __init__(self, socket):
        self.streams_out = []
        self.streams_in = []
        self.write_buffer = bytearray()
        self.socket = socket
        self.socket.setblocking(0)

    def __call__(self):
        return self.socket

    @property
    def is_listening(self):
        try:
            listening = self.socket.getsockopt(socket.SOL_SOCKET, socket.SO_ACCEPTCONN)
        except socket.error:
            listening = -1
        return listening > 0

    def accept(self, ctx):
        """new connections inherit joins on listening socket"""
        conn, address = self.socket.accept()
        managed = ctx.managed_socket(conn)
        for out_stream in self.streams_out:
            JoinStream(ctx, self.socket, out_stream.socket_to(),
                        transform=out_stream.transform,
                        link_close=out_stream.link_close)
        for in_stream in self.streams_in:
            JoinStream(ctx, in_stream.socket_from(), self.socket,
                        transform=in_stream.transform,
                        link_close=in_stream.link_close)

    def pump(self, writables):
        # This is the part that would be optimized by
        # splice or sendfile
        try:
            data = self.socket.recv(4096)
            if data:
                for stream in self.streams_out:
                    if stream.transform is not None:
                        d = stream.transform(data)
                    else:
                        d = data
                    out = stream.socket_to
                    if out() in writables and not out.write_buffer:
                        out().send(d)
                    else:
                        out.write_buffer.extend(d)
            else:
                self.close_ready = True
        except socket.error:
            # lets just assume it died
            self.close_ready = True
            self.write_buffer = bytearray()

    def flush_write_buffer(self):
        assert self.write_buffer
        bytes = self.socket.send(self.write_buffer)
        self.write_buffer = self.write_buffer[bytes:]

    def close(self):
        assert self.close_ready
        assert not self.write_buffer
        for stream in self.streams_out:
            stream.stop()
            if stream.link_close and stream.socket_to:
                stream.socket_to.close_ready = True
        for stream in self.streams_in:
            stream.stop()
            if stream.link_close and stream.socket_from:
                stream.socket_from.close_ready = True
        self.socket.close()


class Context(object):
    instance = None

    sockets = {}
    thread = None
    active = True

    def __init__(self):
        self.thread = threading.Thread(target=self.loop)
        self.thread.start()

    def managed_socket(self, socket):
        if socket not in self.sockets:
            self.sockets[socket] = ManagedSocket(socket)
        return self.sockets[socket]

    def join(self, a, b, transform=None, link_close=True, half_duplex=False):
        if half_duplex:
            JoinStream(self, a, b, transform, link_close)
        else:
            JoinStream(self, a, b, transform, link_close)
            JoinStream(self, b, a, transform, link_close)

    def unjoin(self, a, b):
        if a in self.sockets and b in self.sockets:
            for s in self.sockets[a].streams_out:
                if self.sockets[b] == s.socket_to:
                    s.stop()
            for s in self.sockets[b].streams_in:
                if self.sockets[b] == s.socket_from:
                    s.stop()

    def loop(self):
        while self.active:
            readables, writables, errs = select.select(
                                            self.readables(),
                                            self.writables(),
                                            [], 0)
            
            self.flush_buffered_writables(writables)

            for readable in readables:
                managed = self.managed_socket(readable)
                if managed.is_listening:
                    managed.accept(self)
                else:
                    managed.pump(writables) 

            self.close_finished_sockets()

    def readables(self):
        return [socket() for socket in self.sockets.values() if
                len(socket.streams_out)]

    def writables(self):
        return [socket() for socket in self.sockets.values() if
                len(socket.streams_in)]

    def close_finished_sockets(self):
        for socket in self.sockets.values():
            if socket.close_ready and not socket.write_buffer:
                socket.close()
                self.sockets.pop(socket())

    def flush_buffered_writables(self, writables):
        for socket in self.sockets.values():
            if socket.write_buffer and socket() in writables:
                while len(socket.write_buffer):
                    try:
                        socket.flush_write_buffer()
                    except Exception:
                        break
                
def init():
    #ctx = _context()
    #ctx.thread = threading.Thread(target=_context_loop, args=(ctx,))
    #ctx.thread.start()
    return None

def term(ctx):
    #ctx.active = False
    #ctx.thread.join()
    if Context.instance:
        Context.instance.active = False

def join(ctx, a, b, flags=0, transform=None):
    link_close = not flags & NOCLOSE
    half_duplex = flags & HALFDUPLEX

    if Context.instance is None:
        Context.instance = Context()

    Context.instance.join(a, b, transform, link_close, half_duplex)

def unjoin(ctx, a, b):
    if Context.instance:
        Context.instance.unjoin(a, b)

if False:
    def join(ctx, a, b, flags=0, transform=None):
        def _join(from_, to, callback=None):
            if from_.fileno() not in ctx.joins:
                ctx.joins[from_.fileno()] = ([], [])
            if to.fileno() not in ctx.joins:
                ctx.joins[to.fileno()] = ([], [])
            ctx.joins[from_.fileno()][1].append(to)
            ctx.joins[to.fileno()][0].append(from_)
            if callable(callback):
                ctx.callbacks[(from_.fileno(), to.fileno())] = callback
        
        fda.setblocking(0)
        fdb.setblocking(0)
        if flags & HALFDUPLEX:
            _join(fda, fdb, callback)
        else:
            _join(fda, fdb, callback)
            _join(fdb, fda, callback)

    def unjoin(ctx, fda, fdb, flags=0):
        def _unjoin(from_, to):
            ctx.joins[from_.fileno()][1].remove(to)
            ctx.joins[to.fileno()][0].remove(from_)
            if (from_.fileno(), to.fileno()) in ctx.callbacks:
                ctx.callbacks.pop((from_.fileno(), to.fileno()))

        if flags & HALFDUPLEX:
            _unjoin(fda, fdb)
        else:
            _unjoin(fda, fdb)
            _unjoin(fdb, fda)