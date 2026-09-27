"""exec_chain/chain.py — replace ourselves with sh, which execs ls."""
import os
import sys

d = sys.argv[1]
os.execvp("sh", ["sh", "-c", "exec ls " + d])
