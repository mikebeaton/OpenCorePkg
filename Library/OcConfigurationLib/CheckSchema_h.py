#!/usr/bin/env python

"""
Validates schema header files (e.g. OcConfigurationLib.h) for being sorted.
"""

import re
import sys

if len(sys.argv) < 2:
  print('Pass file to check')
  sys.exit(-1)

with open(sys.argv[1], 'r') as f:
  prev = ''
  content = [l.strip() for l in f.readlines()]
  for i, l in enumerate(content):
    y = re.search(r'#define\s*(\w+)\(', l)
    if y:
      print('Checking schema {}'.format(y.group(1)))
      prev = ''
      continue
    x = re.search(r'_\(\w+\s*,\s*(\w+)', l)
    if x:
      if x.group(1) < prev:
        print('ERROR: {} precedes {}'.format(prev, x.group(1)))
        sys.exit(1)
      prev = x.group(1)
