"""A small asyncio HTTP server (like uvicorn's accept/read/write pattern) used to load the NEOS TCP stack from the host."""
import asyncio, sys
BODY = b"x" * 60000
async def handle(r, w):
    try:
        while True:
            line = await r.readline()
            if not line: break
            while (h := await r.readline()) not in (b"\r\n", b"\n", b""): pass
            w.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: text/plain\r\n\r\n" % len(BODY) + BODY)
            await w.drain()
    except Exception as e: print("handler", type(e).__name__, e, flush=True)
    finally:
        try: w.close()
        except Exception: pass
async def main():
    s = await asyncio.start_server(handle, "0.0.0.0", 8800, backlog=128)
    print("LOAD server ready", flush=True)
    await asyncio.sleep(1500)
asyncio.run(main())
