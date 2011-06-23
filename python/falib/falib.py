'''Simple FA capture library for reading data from the FA sniffer.'''

DEFAULT_SERVER = 'fa-archiver.diamond.ac.uk'
DEFAULT_PORT = 8888

import socket
import struct
import numpy
import cothread


__all__ = [
    'connection', 'subscription', 'get_sample_frequency', 'get_decimation',
    'Server']


MASK_SIZE = 256         # Number of possible bits in a mask

def normalise_mask(mask):
    nmask = numpy.zeros(MASK_SIZE / 8, dtype=numpy.uint8)
    for i in mask:
        nmask[i // 8] |= 1 << (i % 8)
    return nmask

def format_mask(mask):
    '''Converts a mask (a set of integers in the range 0 to 255) into a 64
    character hexadecimal coded string suitable for passing to the server.'''
    return ''.join(['%02X' % x for x in reversed(mask)])

def count_mask(mask):
    n = 0
    for i in range(MASK_SIZE):
        if mask[i // 8] & (1 << (i % 8)):
            n += 1
    return n

class connection:
    class EOF(Exception):
        pass
    class Error(Exception):
        pass

    def __init__(self, server = DEFAULT_SERVER, port = DEFAULT_PORT):
        self.sock = socket.create_connection((server, port))
        self.sock.setblocking(0)
        self.buf = []

    def close(self):
        self.sock.close()

    def recv(self, block_size = 65536, timeout = 1):
        if not cothread.poll_list(
                [(self.sock.fileno(), cothread.POLLIN)], timeout):
            raise socket.timeout('Receive timeout')
        chunk = self.sock.recv(block_size)
        if not chunk:
            raise self.EOF('Connection closed by server')
        return chunk

    def read_block(self, length):
        result = numpy.empty(length, dtype = numpy.int8)
        rx = 0
        buf = self.buf
        while True:
            l = len(buf)
            if l:
                if rx + l <= length:
                    result.data[rx:rx+l] = buf
                    buf = []
                else:
                    result.data[rx:] = buf[:length - rx]
                    buf = buf[length - rx:]
                rx += l
                if rx >= length:
                    break
            buf = self.recv()
        self.buf = buf
        return result


class subscription(connection):
    '''s = subscription(bpm_list, decimated, server, port)

    Creates a stream connection to the given FA archiver server (or the default
    server if not specified) returning continuous data for the selected bpms.
    The s.read() method must be called frequently enough to ensure that the
    connection to the server doesn't overflow.
    '''

    def __init__(self, mask, decimated=False, uncork=False, **kargs):
        connection.__init__(self, **kargs)
        self.mask = normalise_mask(mask)
        self.count = count_mask(self.mask)
        self.decimated = decimated

        flags = ''
        if uncork: flags = flags + 'U'
        if decimated: flags = flags + 'D'
        self.sock.send('SR%s%s\n' % (format_mask(self.mask), flags))
        c = self.recv(1)
        if c != chr(0):
            raise self.Error((c + self.recv())[:-1])    # Discard trailing \n

    def read(self, samples):
        '''Returns a waveform of samples indexed by sample count, bpm count
        (from the original subscription) and channel, thus:
            wf = s.read(N)
        wf[n, b, x] = sample n of BPM b on channel x, where x=0 for horizontal
        position and x=1 for vertical position.'''
        raw = self.read_block(8 * samples * self.count)
        array = numpy.frombuffer(raw, dtype = numpy.int32)
        return array.reshape((samples, self.count, 2))


def server_command(command, **kargs):
    server = connection(**kargs)
    server.sock.send(command)
    result = server.recv()
    server.close()
    return result


def get_sample_frequency(**kargs):
    return float(server_command('CF\n', **kargs))

def get_decimation(**kargs):
    return int(server_command('CC\n', **kargs))


class Server:
    '''A simple helper class to gather together the information required to
    identify the requested server and act as a proxy for the useful commands in
    this module.'''

    def __init__(self, server = DEFAULT_SERVER, port = DEFAULT_PORT):
        self.server = server
        self.port = port

        response = self.server_command('CFC\n').split('\n')
        self.sample_frequency = float(response[0])
        self.decimation = int(response[1])

    def server_command(self, command):
        return server_command(command, server = self.server, port = self.port)

    def subscription(self, mask, **kargs):
        return subscription(
            mask, server = self.server, port = self.port, **kargs)
