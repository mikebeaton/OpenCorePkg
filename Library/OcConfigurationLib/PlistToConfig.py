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

def plist_schema_attr_print(name, value):
  if value is not None:
    info_print(' ', name, '="', value, '"', info_flags=PRINT_PLIST_SCHEMA, end='')

def oc_schema_attr_print(name, value):
  if value is not None:
    info_print(' ', name, '="', value, '"', info_flags=PRINT_OC_SCHEMA, end='')

@dataclass
class PlistSchemaElement:
  type: str
  data_type: str
  data_size: str
  value: str
  default: str
  of: object

  def __init__(
    self,
    type: str,
    data_type: str = None,
    data_size: str = None,
    value: str = None,
    default: str = None,
    of: object = None
    ):

    self.type = type
    self.data_type = data_type
    self.data_size = data_size
    self.value = value
    self.default = default
    self.of = of

    plist_schema_print('[plist:', type, end='')
    plist_schema_attr_print('data_type', data_type)
    plist_schema_attr_print('data_size', data_size)
    plist_schema_attr_print('value', value)
    plist_schema_attr_print('default', default)
    plist_schema_attr_print('of', of.data_type if of is not None else None)
    plist_schema_print(']')

@dataclass
class OcSchemaElement:
  type: str
  name: str
  data_type: str
  data_size: str
  value: str
  default: str
  of: object

  def __init__(
    self,
    type: str,
    name: str = None,
    data_type: str = None,
    data_size: str = None,
    value: str = None,
    default: str = None,
    of: object = None
    ):

    self.type = type
    self.name = name
    self.data_type = data_type
    self.data_size = data_size
    self.value = value
    self.default = default
    self.of = of

    oc_schema_print('[OC:', type, end='')
    oc_schema_attr_print('name', name)
    oc_schema_attr_print('data_type', data_type)
    oc_schema_attr_print('data_size', data_size)
    oc_schema_attr_print('value', value)
    oc_schema_attr_print('default', default)
    oc_schema_attr_print('of', of.data_type if of is not None else None)
    oc_schema_print(']')

def plist_open(elem, tab):
  plist_print(tab, '<', elem.tag, '>')

def plist_close(elem, tab):
  plist_print(tab, '</', elem.tag, '>')

def plist_open_close(elem, tab):
  if elem.text is not None:
    plist_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
  else:
    plist_print(tab, '<', elem.tag, '/>')

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

  return PlistSchemaElement(elem.tag, data_type=type.upper(), data_size=size, value=data_print)

def parse_array(elem, tab):
  plist_open(elem, tab)
  count = len(elem)
  if count == 0:
    error('No template for <array>')
  child = parse_elem(elem[0], tab)
  if (count > 1):
    plist_print(tab, '\t(skipping ', count - 1, ' item', '' if (count - 1) == 1 else 's' , ')')
  plist_close(elem, tab)

  return OcSchemaElement(type='ARRAY', of=child)

def init_dict(elem, tab, map):
  displayName = '<' + elem.tag + (' type="map"' if map else '') + '>'

  plist_print(tab, displayName)

  count = len(elem)

  if count == 0:
    error('No elements in ', displayName)

  if count % 2 != 0:
    error('Number of nodes in ', displayName, ' must be even')

  return count >> 1

def check_key(parent, child, index):
  if child.type != 'key':
    error('<key> required as ', 'first' if index == 0 else 'every even' , ' element of <', parent.tag, '>')

def parse_map(elem, tab):
  count = init_dict(elem, tab, True)

  key = parse_elem(elem[0], tab)

  check_key(elem, key, 0)

  value = parse_elem(elem[1], tab)

  count -= 1

  if (count > 0):
    plist_print(tab, '\t(skipping ', count, ' item', '' if count == 1 else 's' , ')')

  plist_close(elem, tab)

  return OcSchemaElement(type='OC_MAP', of=value)

def parse_fields(elem, tab):
  count = init_dict(elem, tab, False)

  index = 0
  while count > 0:
    key = parse_elem(elem[index], tab)
    check_key(elem, key, index)

    parse_elem(elem[index + 1], tab)

    count -= 1
    index += 2

  plist_close(elem, tab)

  return OcSchemaElement(type='OC_FIELDS', of=None)

def parse_plist(elem, tab):
  plist_open(elem, tab)

  count = len(elem)
  if count != 1:
    error('Invalid contents for <plist>')

  child = parse_elem(elem[0], tab, False)

  plist_close(elem, tab)

  return child

def parse_elem(elem, tab, indent = True):
  if tab == None:
    tab =''

  if indent:
    tab += '\t'

  if elem.tag == 'true' or elem.tag == 'false':
    plist_open_close(elem, tab)
    return OcSchemaElement(type='BOOLEAN', value=elem.tag)

  if elem.tag == 'key':
    plist_open_close(elem, tab)
    return PlistSchemaElement(type=elem.tag, value=elem.text)

  if elem.tag == 'string':
    plist_open_close(elem, tab)
    return OcSchemaElement(type='OC_STRING', value=elem.text)

  if elem.tag == 'integer':
    plist_open_close(elem, tab)
    return OcSchemaElement(type='UINT32', value=elem.text)

  if elem.tag == 'data':
    return parse_data(elem, tab)

  if elem.tag == 'array':
    return parse_array(elem, tab)

  if elem.tag == 'dict':
    if 'type' in elem.attrib and elem.attrib['type'] == 'map':
      return parse_map(elem, tab)
    else:
      return parse_fields(elem, tab)

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

parse_elem(root, None, False)

file_close(c_file)
file_close(h_file)
file_close(o_file)

sys.exit(0)
