"""Client side of ScummVM's Archipelago bridge (doc/archipelago-bridge.md).

Game-agnostic: an Archipelago client subclasses nothing here; it creates a
BridgeServer, registers rules from its game definition, and reacts to matches.

    async def main():
        bridge = BridgeServer(port=38282)
        bridge.on_hello = lambda hello: bridge.watch(1, 'send', ['*', 'givePoints'])
        bridge.on_match = lambda ev: print('check', ev)
        await bridge.serve()
"""
import asyncio
import json
import itertools


class BridgeError(Exception):
    pass


class BridgeServer:
    def __init__(self, host='127.0.0.1', port=38282):
        self.host, self.port = host, port
        self._writer = None
        self._pending = {}
        self._req = itertools.count(1)
        self.hello = None
        self.on_hello = None    # callable(dict): (re)send rules here
        self.on_match = None    # callable(dict): a watch rule fired
        self.on_trace = None    # callable(dict): traced event
        self.on_bye = None
        self._tasks = set()

    @property
    def connected(self):
        return self._writer is not None

    async def serve(self):
        server = await asyncio.start_server(self._handle, self.host, self.port)
        async with server:
            await server.serve_forever()

    async def _handle(self, reader, writer):
        self._writer = writer
        try:
            while line := await reader.readline():
                msg = json.loads(line)
                kind = msg.get('type')
                if kind == 'result' and msg.get('req') in self._pending:
                    self._pending.pop(msg['req']).set_result(msg)
                elif kind == 'hello':
                    self.hello = msg
                    self._dispatch(self.on_hello, msg)
                elif kind == 'match':
                    self._dispatch(self.on_match, msg)
                elif kind == 'trace':
                    self._dispatch(self.on_trace, msg)
                elif kind == 'bye':
                    self._dispatch(self.on_bye, msg)
        finally:
            self._writer = None
            for fut in self._pending.values():
                fut.cancel()
            self._pending.clear()

    def _dispatch(self, callback, msg):
        # Callbacks run as their own tasks so they can await do() without
        # blocking the reader that delivers do()'s reply.
        if not callback:
            return
        result = callback(msg)
        if asyncio.iscoroutine(result):
            task = asyncio.ensure_future(result)
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

    def send(self, **msg):
        if not self._writer:
            raise BridgeError('game not connected')
        self._writer.write((json.dumps(msg) + '\n').encode())

    # -- rules -----------------------------------------------------------
    def watch(self, rule, kind, keys=(), args=(), ctx=None, veto=False, value=0, once=False):
        self.send(cmd='watch', rule=rule, kind=kind, keys=list(keys), args=list(args),
                  ctx=ctx or {}, veto=veto, value=value, once=once)

    def unwatch(self, rule):
        self.send(cmd='unwatch', rule=rule)

    def clear(self):
        self.send(cmd='clear')

    def trace(self, kind, on=True):
        self.send(cmd='trace', kind=kind, on=on)

    def say(self, text):
        self.send(cmd='say', text=text)

    # -- actions ---------------------------------------------------------
    async def do(self, op, keys=(), args=(), timeout=10.0):
        """Run an engine action; returns its integer result or raises BridgeError."""
        req = next(self._req)
        fut = asyncio.get_running_loop().create_future()
        self._pending[req] = fut
        self.send(cmd='do', req=req, op=op, keys=list(keys), args=list(args))
        msg = await asyncio.wait_for(fut, timeout)
        if not msg.get('ok'):
            raise BridgeError(msg.get('error', 'action failed'))
        return msg.get('value', 0)


if __name__ == '__main__':
    # Discovery mode: print every event of the given kinds, e.g.
    #   python3 apbridge.py owner room      (SCUMM: find item pickups)
    #   python3 apbridge.py call            (SCI: find flag procedures)
    import sys
    kinds = sys.argv[1:] or ['room']
    bridge = BridgeServer()

    def hello(h):
        print('connected:', h)
        for k in kinds:
            bridge.trace(k)

    bridge.on_hello = hello
    bridge.on_trace = lambda ev: print(ev['kind'], ev['keys'], ev['args'], ev['ctx'])
    asyncio.run(bridge.serve())
