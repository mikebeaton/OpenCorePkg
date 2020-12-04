#!/usr/bin/env python3

"""
Generate OpenCore .c and .h plist definition files from template plist file.
"""

import base64
import io
import os
import sys
import xml.etree.ElementTree as ET

from dataclasses import dataclass

# output files
c_file = None
h_file = None
o_file = None

# support customisation for other apps
DEFAULT_PREFIX = 'Oc'

camelPrefix = None
upperPrefix = None

# show markup with implied types added
PRINT_PLIST = 1 << 0
# show creation of plist schema objects
PRINT_PLIST_SCHEMA = 1 << 1
# show creation of OC schema objects
PRINT_OC_SCHEMA = 1 << 2
# Show extra debug info
SHOW_DEBUG = 1 << 3

flags = PRINT_PLIST

def error(*args, **kwargs):
  print('ERROR: ', *args, sep='', file=sys.stderr, **kwargs)
  sys.exit(-1)

def debug(*args, **kwargs):
  if flags & SHOW_DEBUG != 0:
    print('DEBUG: ', *args, sep='', file=sys.stderr, **kwargs)

def info_print(*args, **kwargs):
  if kwargs.pop('info_flags', 0) & flags != 0:
    print(*args, file=o_file, sep='', **kwargs)

def plist_print(*args, **kwargs):
  info_print(*args, info_flags=PRINT_PLIST, **kwargs)

def plist_schema_print(*args, **kwargs):
  info_print(*args, info_flags=PRINT_PLIST_SCHEMA, **kwargs)

def oc_schema_print(*args, **kwargs):
  info_print(*args, info_flags=PRINT_OC_SCHEMA, **kwargs)

@dataclass
class OcSchemaElement:
  OcType: str
  OcName: str = None
  OcSize: str = None
  OcValue: str = None
  OcOf: object = None

  def __init__(
    self,
    OcType: str,
    OcName: str = None,
    OcSize: str = None,
    OcValue: str = None,
    OcOf: object = None
    ):

    self.OcType = OcType
    self.OcName = OcName
    self.OcSize = OcSize
    self.OcValue = OcValue
    self.OcOf = OcOf
    
    oc_schema_print('[', OcType, end='')
    if OcName is not None: oc_schema_print(' name="', OcName, '"', end='')
    if OcSize is not None: oc_schema_print(' size="', OcSize, '"', end='')
    if OcValue is not None: oc_schema_print(' value="', OcValue, '"', end='')
    if OcOf is not None: oc_schema_print(' of=', OcOf.OcType, end='')
    oc_schema_print(']')

def parse_data(elem, tab):
  type = elem.attrib['type'] if 'type' in elem.attrib else None
  size = elem.attrib['size'] if 'size' in elem.attrib else None
  data = elem.text

  if data is not None:
    data_bytes = base64.b64decode(data)
    data_print = '0x' + data_bytes.hex()
  else:
    data_bytes = None
    data_print = None

  if data is not None and (type is None or size is None):
    length = len(data_bytes)

    type_from_data = None

    if length == 2:
      type_from_data = 'uint16'
    elif length == 4:
      type_from_data = 'uint32'
    elif length == 8:
      type_from_data = 'uint64'
    else:
      type_from_data = 'uint8'
      if length != 1 and size is None:
        size = length

    if type is None:
      type = type_from_data

  if type is None:
    if size is None:
      type = 'blob'
    else:
      type = 'uint8'
  elif type == 'blob' and size is not None:
    error('size attribute not valid with type="blob"')

  plist_print(tab, '<', elem.tag, end='')
  plist_print(' type="', type, end='')
  if size is not None:
    plist_print('" size="', size, end='')
  plist_print('">', data_print if data_print is not None else '[None]', '</', elem.tag, '>')

  return OcSchemaElement(OcType=type.upper(), OcSize=size, OcValue=data_print)

def parse_array(elem, tab):
  next_tab = tab + '\t'
  plist_print(tab, '<', elem.tag, '>')
  count = len(elem)
  if count == 0:
    error('No template for <array>')
  child = parse_elem(elem[0], next_tab)
  if (count > 1):
    plist_print(next_tab, '(skipping ', count - 1, ' item', '' if (count - 1) == 1 else 's' , ')')
  plist_print(tab, '</', elem.tag, '>')

  return OcSchemaElement(OcType='ARRAY', OcOf=child)

def parse_map_struct(elem, tab):
  isMap = True if 'type' in elem.attrib and elem.attrib['type'] == 'map' else False

  displayName = elem.tag
  if isMap: displayName += ' type="map"'

  next_tab = tab + '\t'

  plist_print(tab, '<', displayName, '>')

  count = len(elem)
  if count == 0:
    error('No elements in <', displayName ,'>')
  if count % 2 != 0:
    error('Number of nodes in <', displayName, '> must be even')
  count = count >> 1

  index = 0
  while True:
    key = parse_elem(elem[index], next_tab)
    if key.OcType != 'key':
      error('<key> required as first element of <dict type="map">')
    value = parse_elem(elem[index + 1], next_tab)
    child = OcSchemaElement(
      OcType = value.OcType,
      OcName = key.OcName,
      OcSize = value.OcSize,
      OcValue = value.OcValue,
      OcOf = value.OcOf
      )
    count -= 1
    index += 2
    if isMap or count == 0:
      break

  if (count > 0):
    plist_print(next_tab, '(skipping ', count, ' item', '' if count == 1 else 's' , ')')

  plist_print(tab, '</', elem.tag, '>')

  if isMap:
    return OcSchemaElement(OcType='OC_MAP', OcOf=value)
  else:
    return OcSchemaElement(OcType='OC_STRUCT', OcOf=None)

def parse_plist(elem, tab):
  plist_print(tab, '<', elem.tag, '>')
  count = len(elem)
  if count != 1:
    error('Invalid contents for <plist>')
  child = parse_elem(elem[0], tab)
  plist_print(tab, '</', elem.tag, '>')
  return child

def parse_elem(elem, tab):
  if elem.tag == 'true' or elem.tag == 'false':
    plist_print(tab, '<', elem.tag, '/>')
    return OcSchemaElement(OcType='BOOLEAN', OcValue=elem.tag)

  if elem.tag == 'key':
    plist_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
    return OcSchemaElement(OcType=elem.tag, OcName=elem.text)

  if elem.tag == 'string':
    plist_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
    return OcSchemaElement(OcType='OC_STRING', OcValue=elem.text)

  if elem.tag == 'integer':
    plist_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
    return OcSchemaElement(OcType='UINT32', OcValue=elem.text)

  if elem.tag == 'data':
    return parse_data(elem, tab)

  if elem.tag == 'array':
    return parse_array(elem, tab)

  if elem.tag == 'dict':
    return parse_map_struct(elem, tab)

  if elem.tag == 'plist':
    return parse_plist(elem, tab)

  error('Unhandled tag:', elem.tag)

def file_close(handle):
  if handle != devnull and handle != sys.stdout:
    debug('Closing: ', handle)
    handle.close()

def twice(flag, handle):
  if handle is not None:
    error(flag, ' specified twice')

##
# Usage, input args, general init
#

argc = len(sys.argv)
if argc < 2:
  print('PlistToConfig [-c c-file] [-h h-file] [-o stdout-file] [-f print-flags] [-p prefix] plist-file')
  sys.exit(-1)

c_structors = io.StringIO()
c_schema = io.StringIO()
h_types = io.StringIO()

plist_filename = None

skip = False
for i in range(1, argc):

  if skip:
    
    skip = False
    continue

  arg = sys.argv[i]
  debug('arg[', i, '] = ', arg)
  skip = True

  if arg == '-f' or arg == '-p':

    if i + 1 >= argc:
      error('Missing value for ', arg, ' flag')

    if arg == '-f':

      flags = int(sys.argv[i + 1])
      debug('flags = ', flags)

    elif arg == '-p':

      camelPrefix = sys.argv[i + 1]
      debug('prefix = ', camelPrefix)

    else:

      error('internal flag error')

  elif arg == '-c' or arg == '-h' or arg == '-o':

    if i + 1 >= argc:
      error('Missing file for ', arg)

    flag_filename = sys.argv[i + 1]

    debug('filename = \'', flag_filename, '\'')
    if flag_filename == '-':
      handle = sys.stdout
    elif flag_filename.startswith('-'):
      error('Missing file for ', arg)
    else:
      handle = open(flag_filename, 'w')

    debug(arg, ' ', handle)

    if arg == '-c':
      twice(arg, c_file)
      c_file = handle
    elif arg == '-h':
      twice(arg, h_file)
      h_file = handle
    elif arg == '-o':
      twice(arg, o_file)
      o_file = handle
    else:
      error('internal file flag error')

  elif not arg.startswith('-'):

    if plist_filename is not None:
      error('"', arg, '": too many input files, already using "', plist_filename, '"')

    plist_filename = arg
    skip = False

  else:

    error('Unknown flag ', arg)

if plist_filename is None:
  error('No input file')

debug('Reading XML from \'', plist_filename, '\'')
root = ET.parse(plist_filename).getroot()

if camelPrefix is None:
  camelPrefix = DEFAULT_PREFIX
upperPrefix = camelPrefix.upper()

if c_file is None or h_file is None:
  devnull = open(os.devnull, 'w')

if c_file is None:
  c_file = devnull

if h_file is None:
  h_file = devnull

if o_file is None:
  o_file = sys.stdout

parse_elem(root, '')

file_close(c_file)
file_close(h_file)
file_close(o_file)

sys.exit(0)
