#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check command lifetime and ACK handling in the patched bridger helper."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "package/network/services/bridger/patches/100-propagate-netlink-errors.patch"

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define NLE_NOMEM 5
#define NL_STOP 2
#define NL_CB_ACK 1
#define NL_CB_CUSTOM 1
struct nl_msg { int unused; };
struct nl_sock { int unused; };
struct nl_cb { int (*ack)(struct nl_msg *, void *); void *arg; };
static struct nl_sock sock, *cmd_sock = &sock;
static struct nl_cb callback;
static int send_result, receive_error, sends, receives, frees, puts;
static bool clone_failure;
static struct nl_cb *nl_socket_get_cb(struct nl_sock *s) { return &callback; }
static struct nl_cb *nl_cb_clone(struct nl_cb *cb) { return clone_failure ? NULL : cb; }
static void nl_cb_set(struct nl_cb *cb, int type, int kind,
		      int (*fn)(struct nl_msg *, void *), void *arg)
{
	cb->ack = fn;
	cb->arg = arg;
}
static int nl_send_auto_complete(struct nl_sock *s, struct nl_msg *msg)
{
	sends++;
	return send_result;
}
static void nlmsg_free(struct nl_msg *msg) { frees++; }
static void nl_cb_put(struct nl_cb *cb) { cb->arg = NULL; puts++; }
static int nl_recvmsgs(struct nl_sock *s, struct nl_cb *cb)
{
	receives++;
	assert(receives <= 2);
	if (receive_error)
		return receive_error;
	/* A query returns data before its command acknowledgment. */
	if (receives == 2)
		assert(cb->ack(NULL, cb->arg) == NL_STOP);
	return 0;
}
"""

CASES = r"""
static void reset(void)
{
	sends = receives = frees = puts = 0;
	clone_failure = false;
	send_result = 64;
	receive_error = 0;
}
int main(void)
{
	struct nl_msg msg;
	reset();
	assert(!bridger_nl_request(&msg));
	assert(sends == 1 && receives == 2 && frees == 1 && puts == 1);
	reset();
	send_result = -7;
	assert(bridger_nl_request(&msg) == -7);
	assert(sends == 1 && receives == 0 && frees == 1 && puts == 1);
	reset();
	clone_failure = true;
	assert(bridger_nl_request(&msg) == -NLE_NOMEM);
	assert(!sends && !receives && frees == 1 && !puts);
	reset();
	assert(bridger_nl_request(NULL) == -NLE_NOMEM);
	assert(!sends && !receives && !frees && !puts);
	reset();
	receive_error = -9;
	assert(bridger_nl_request(&msg) == -9);
	assert(sends == 1 && receives == 1 && frees == 1 && puts == 1);
	return 0;
}
"""


class RequestTest(unittest.TestCase):
    def test_ack_and_failure_paths(self):
        added = "".join(
            line[1:] for line in PATCH.read_text().splitlines(keepends=True)
            if line.startswith("+") and not line.startswith("+++")
        )
        functions = []
        for name in ("bridger_nl_ack_cb", "bridger_nl_request"):
            start = added.index("static int\n" + name + "(")
            end = added.index("\n}", start) + 3
            functions.append(added[start:end])
        with tempfile.TemporaryDirectory(prefix="bridger-request-") as directory:
            source = Path(directory) / "request.c"
            binary = Path(directory) / "request"
            source.write_text(HARNESS + "\n".join(functions) + CASES)
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc")) +
                ["-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 str(source), "-o", str(binary)], check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
