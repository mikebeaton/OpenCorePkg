#!/usr/bin/env python3

"""
Generate OpenCore .c and .h plist definition files from template plist file.
"""

import base64
import io
import sys
import xml.etree.ElementTree as ET

from dataclasses import dataclass

def error(*args, **kwargs):
  print('ERROR: ', *args, sep='', file=sys.stderr, **kwargs)
  sys.exit(-1)

# show markup with implied types added
def stdout_print(*args, **kwargs):
  print(*args, file=stdout_handle, sep='', **kwargs)

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
    stdout_print('[', OcType, end='')
    if OcName is not None: stdout_print(' name="', OcName, '"', end='')
    if OcSize is not None: stdout_print(' size="', OcSize, '"', end='')
    if OcValue is not None: stdout_print(' value="', OcValue, '"', end='')
    if OcOf is not None: stdout_print(' of=', OcOf.OcType, end='')
    stdout_print(']')

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

  stdout_print(tab, '<', elem.tag, end='')
  stdout_print(' type="', type, end='')
  if size is not None:
    stdout_print('" size="', size, end='')
  stdout_print('">', data_print if data_print is not None else '[None]', '</', elem.tag, '>')

  return OcSchemaElement(OcType=type.upper(), OcSize=size, OcValue=data_print)

def parse_array(elem, tab):
  next_tab = tab + '\t'
  stdout_print(tab, '<', elem.tag, '>')
  count = len(elem)
  if count == 0:
    error('No template for <array>')
  child = parse_elem(elem[0], next_tab)
  if (count > 1):
    stdout_print(next_tab, '(skipping ', count - 1, ' item', '' if (count - 1) == 1 else 's' , ')')
  stdout_print(tab, '</', elem.tag, '>')

  return OcSchemaElement(OcType='ARRAY', OcOf=child)

def parse_map_struct(elem, tab):
  isMap = True if 'type' in elem.attrib and elem.attrib['type'] == 'map' else False

  displayName = elem.tag
  if isMap: displayName += ' type="map"'

  next_tab = tab + '\t'

  stdout_print(tab, '<', displayName, '>')

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
    stdout_print(next_tab, '(skipping ', count, ' item', '' if count == 1 else 's' , ')')

  stdout_print(tab, '</', elem.tag, '>')

  if isMap:
    return OcSchemaElement(OcType='OC_MAP', OcOf=value)
  else:
    return OcSchemaElement(OcType='OC_STRUCT', OcOf=None)

def parse_plist(elem, tab):
  stdout_print(tab, '<', elem.tag, '>')
  count = len(elem)
  if count != 1:
    error('Invalid contents for <plist>')
  child = parse_elem(elem[0], tab)
  stdout_print(tab, '</', elem.tag, '>')
  return child

def parse_elem(elem, tab):
  if elem.tag == 'true' or elem.tag == 'false':
    stdout_print(tab, '<', elem.tag, '/>')
    return OcSchemaElement(OcType='BOOLEAN', OcValue=elem.tag)

  if elem.tag == 'key':
    stdout_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
    return OcSchemaElement(OcType=elem.tag, OcName=elem.text)

  if elem.tag == 'string':
    stdout_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
    return OcSchemaElement(OcType='OC_STRING', OcValue=elem.text)

  if elem.tag == 'integer':
    stdout_print(tab, '<', elem.tag, '>', elem.text, '</', elem.tag, '>')
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

# main():

if len(sys.argv) < 4:
  print('args: plist-file c-file h-file [stdout-file]')
  sys.exit(-1)

c_structors = io.StringIO()
c_schema = io.StringIO()
h_types = io.StringIO()

root = ET.parse(sys.argv[1]).getroot()

stdout_handle = open(sys.argv[4], 'w') if len(sys.argv) > 4 else sys.stdout

camelPrefix = root.attrib('prefix') if 'prefix' in root.attrib else 'Oc'
upperPrefix = camelPrefix.upper()

parse_elem(root, '')
