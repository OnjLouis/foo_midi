"""Exercise the real VST host IPC with a deterministic, ROM-free test synth."""
import math
import os
import queue
import struct
import subprocess
import sys
import threading


class Host:
    def __init__(self, host, plugin):
        plugin = os.path.abspath(plugin)
        cookie = sum((ord(c) * 820109) & 0xFFFF for c in plugin) & 0xFFFFFFFF
        self.process = subprocess.Popen([host, plugin, f"{cookie:x}"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.output = queue.Queue()
        def reader():
            while True:
                data = self.process.stdout.read(1)
                if not data:
                    self.output.put(None)
                    return
                self.output.put(data)
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()
        try:
            if self.code() != 0:
                raise AssertionError("Host initialization failed")
            info = [self.code() for _ in range(6)]
            self.read(sum(info[:3]))
        except BaseException:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=10)
            self.reader.join(timeout=2)
            raise

    def read(self, size):
        data = bytearray()
        for _ in range(size):
            item = self.output.get(timeout=10)
            if item is None:
                raise AssertionError(f"Host ended unexpectedly: {self.process.poll()}")
            data.extend(item)
        return data

    def code(self):
        return struct.unpack("<I", self.read(4))[0]

    def command(self, *codes, payload=b""):
        self.process.stdin.write(struct.pack("<" + "I" * len(codes), *codes) + payload)
        self.process.stdin.flush()
        if self.code() != 0:
            raise AssertionError("Command failed")

    def event(self, port, status, first, second=0, frame=None):
        event = (port << 24) | status | (first << 8) | (second << 16)
        self.command(7, event) if frame is None else self.command(10, event, frame)

    def sysex(self, port, program, frame=None):
        payload = bytes([0xF0, 0x7D, program, 0xF7])
        codes = (8, (port << 24) | len(payload)) if frame is None else (11, (port << 24) | len(payload), frame)
        self.command(*codes, payload=payload)

    def render(self, frames=16):
        self.command(9, frames)
        samples = struct.unpack("<" + "f" * (frames * 2), self.read(frames * 8))
        return list(zip(samples[::2], samples[1::2]))

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.write(struct.pack("<I", 0))
            self.process.stdin.flush()
            self.process.wait(timeout=10)
        self.reader.join(timeout=2)
        assert not self.reader.is_alive(), "Pipe reader did not exit"
        assert self.process.returncode == 0, "Host did not shut down cleanly"


def check(value, expected):
    assert math.isclose(value, expected, abs_tol=1e-6), (value, expected)


def main(host_path, plugin_path):
    host = None
    try:
        host = Host(host_path, plugin_path)
        host.command(5, 4, 48000)
        host.event(0, 0xC0, 10)
        host.event(0, 0x90, 60, 100)
        left, right = host.render()[0]
        check(left, .11)
        check(right, .01)  # A one-port file must not allocate idle synth copies.
        for port, program in [(1,20), (2,30), (3,40)]:
            host.event(port, 0xC0, program)
            host.event(port, 0x90, 60, 100)
        left, right = host.render()[0]
        check(left, 1.04)  # Four independently selected instruments, summed.
        check(right, .16)
        host.event(3, 0x80, 60, 0)
        check(host.render()[0][0], .63)  # Only port D stopped.
        host.sysex(3, 50, frame=4)
        samples = host.render()
        check(samples[3][0], .63)
        check(samples[4][0], 1.14)
        host.command(6)  # Recreate primary; old auxiliary voices must disappear.
        check(host.render()[0][0], 0)
        host.event(0, 0x90, 60, 100, frame=4)
        samples = host.render()
        check(samples[3][0], 0)
        check(samples[4][0], .01)
        check(samples[4][1], .01)
        host.event(127, 0x90, 60, 100)  # Unsupported ports must not alias port D.
        check(host.render()[0][0], .01)
        host.sysex(127, 80)
        check(host.render()[0][0], .01)
        host.command(1)  # Copy state through the same protocol the player uses.
        chunk = host.read(host.code())
        host.command(2, len(chunk), payload=chunk)
        for cycle in range(100):
            host.command(6)
            for port in range(4):
                host.event(port, 0xC0, 10 * (port + 1))
                host.event(port, 0x90, 60, 100)
                host.sysex(port, 10 * (port + 1))
            check(host.render()[0][0], 1.04)
        host.command(6)
        host.command(5, 4, 44100)
        host.event(0, 0x90, 60, 100)
        check(host.render()[0][1], .01)
        print("VST port regression tests passed")
        host.close()
    finally:
        if host is not None and host.process.poll() is None:
            host.process.kill()
            host.process.wait(timeout=10)
            host.reader.join(timeout=2)


if __name__ == "__main__":
    main(*sys.argv[1:])
