"""threads/worker.py — 4 threads, ordered milestones, real overlap.

t0 runs immediately; t1..t3 block on forward pipes. Each thread opens its
own file, signals the next, then blocks on its ack pipe until its successor
is done — so opens land f0..f3 and deaths land t3,t2,t1,t0. The main thread
polls file existence (stat syscalls are invisible to the tracer) between
pthread creations so proc.start order is total too.
"""
import os
import sys
import threading

d = sys.argv[1]

pipes = [os.pipe() for _ in range(3)]  # forward: t0->t1, t1->t2, t2->t3
acks = [os.pipe() for _ in range(3)]  # backward: t1->t0, t2->t1, t3->t2


def work(i, wait_fd, signal_fd, ack_wait_fd, ack_signal_fd):
    if wait_fd >= 0:
        os.read(wait_fd, 1)
    with open(os.path.join(d, "t%d" % i), "w") as f:
        f.write("thread %d\n" % i)
    if signal_fd >= 0:
        os.write(signal_fd, b"x")
    if ack_wait_fd >= 0:
        os.read(ack_wait_fd, 1)
    if ack_signal_fd >= 0:
        os.write(ack_signal_fd, b"x")


specs = [
    (0, -1, pipes[0][1], acks[0][0], -1),
    (1, pipes[0][0], pipes[1][1], acks[1][0], acks[0][1]),
    (2, pipes[1][0], pipes[2][1], acks[2][0], acks[1][1]),
    (3, pipes[2][0], -1, -1, acks[2][1]),
]

threads = []
for i, wf, sf, awf, asf in specs:
    t = threading.Thread(target=work, args=(i, wf, sf, awf, asf))
    t.start()
    threads.append(t)
    # Order creation milestones: f{i} exists only after t{i}'s open returned,
    # and the open's exit-stop precedes that return, so the recorded event is
    # already in the trace before the next clone.
    while not os.path.exists(os.path.join(d, "t%d" % i)):
        pass
for t in threads:
    t.join()
