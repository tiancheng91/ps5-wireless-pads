"""Run the production USB pump with fake completions, without PS5 USB headers."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/hci_usb.c").read_text()
# Compile the actual function, rather than a copy that could drift from it.
pump = source.split("static int op_pump(", 1)[1].split(
    "\nstatic int op_next_event(", 1
)[0]
harness = r"""
#include <assert.h>
#include <stdio.h>

typedef struct { int count; } queue;
typedef struct { int dead; queue events, acl; } usb_ctx;
enum { IDLE, EMPTY_COMPLETIONS, EVENT, ACL, LOST };
static int mode, polls, sleeps;
static long clock_ms;
static long now_ms(void) { return clock_ms; }
static void keep_reads_pending(usb_ctx *c) { (void)c; }
static void usleep(unsigned int us) { clock_ms += us / 1000; sleeps++; }
static int reap(usb_ctx *c)
{
    polls++;
    clock_ms++;
    /* Bound the broken loop too, so a regression fails instead of hanging. */
    if (polls >= 100) c->dead = 1;
    if (mode == EVENT) c->events.count = 1;
    if (mode == ACL) c->acl.count = 1;
    if (mode == LOST) c->dead = 1;
    return mode != IDLE;
}
"""
harness += "\nstatic int op_pump(" + pump
harness += r"""
static void reset(int m)
{
    mode = m;
    polls = sleeps = 0;
    clock_ms = 0;
}
int main(void)
{
    usb_ctx c = {0};
    reset(EMPTY_COMPLETIONS);
    assert(op_pump(&c, 4) == 0);
    assert(clock_ms == 4 && polls == 4 && sleeps == 0);
    assert(!c.dead); /* A timeout must not disconnect the transport. */

    reset(IDLE);
    assert(op_pump(&c, 4) == 0);
    assert(clock_ms >= 4 && clock_ms <= 5 && sleeps > 0);

    reset(EMPTY_COMPLETIONS);
    assert(op_pump(&c, 0) == 0 && polls == 1);

    reset(EVENT);
    assert(op_pump(&c, 0) == 1 && c.events.count == 1);
    c.events.count = 0;
    reset(ACL);
    assert(op_pump(&c, 4) == 1 && c.acl.count == 1 && polls == 1);
    c.acl.count = 0;

    reset(LOST);
    assert(op_pump(&c, 4) == -1);
    reset(IDLE);
    assert(op_pump(&c, 4) == -1 && polls == 0);
    puts("all USB poll checks passed");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="padbridge-usb-poll-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run(
        shlex.split(os.environ.get("CC", "cc"))
        + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
           str(path / "test.c"), "-o", str(path / "test")],
        check=True,
        timeout=30,
    )
    subprocess.run([str(path / "test")], check=True, timeout=5)
