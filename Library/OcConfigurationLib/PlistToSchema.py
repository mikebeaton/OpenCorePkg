#!/usr/bin/env python3

"""
Generate OpenCore .c and .h plist definition files from template plist.
"""

import base64
import sys
import xml.etree.ElementTree as ET

if len(sys.argv) < 4:
  print('Pass: plist-file c-file h-file')
  sys.exit(-1)

def parse_elem(elem, tabs):
  if elem.tag == 'key' or elem.tag == 'string' or elem.tag == 'integer':
    print(tabs, '<', elem.tag, '>', elem.text, '</', elem.tag, '>', sep='')
  elif elem.tag == 'data':
    type = elem.attrib['type'] if 'type' in elem.attrib else None
    size = elem.attrib['size'] if 'size' in elem.attrib else None
    data = elem.text

    if data is not None:
      data_bytes = base64.b64decode(data)
      data_print = '0x' + data_bytes.hex()
    else:
      data_bytes = None
      data_print = '[None]'

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

    if type is None and size is None:
      type = 'blob'
    
    type_size_err = False
    if type == 'blob':
      type_size_err = size is not None
    else:
      type_size_err = type is None
    if type_size_err:
      print('ERROR: Cannot calculate data type and size')
      sys.exit(-1)

    print(tabs, '<', elem.tag, sep='', end='')
    print(' type="', type, sep='', end='')
    if size is not None:
      print('" size="', size, sep='', end='')
    print('">', data_print, '</', elem.tag, '>', sep='')
  elif elem.tag == 'true' or elem.tag == 'false':
    print(tabs, '<', elem.tag, '/>', sep='')
  else:
    child_tabs = tabs + '    '
    if elem.tag == 'array':
      print(tabs, '<', elem.tag, '>', sep='')
      count = len(elem)
      if count == 0:
        print('WARNING: No template for array')
      if (count > 0):
        parse_elem(elem[0], child_tabs)
      if (count > 1):
        print(child_tabs, '(skipping ', count - 1, ' item', '' if (count - 1) == 1 else 's' , ')', sep='')
      print(tabs, '</', elem.tag, '>', sep='')
    elif elem.tag == 'dict' and 'type' in elem.attrib and elem.attrib['type'] == 'map':
      print(tabs, '<', elem.tag, ' type="map">', sep='')
      count = len(elem)
      if count % 2 != 0:
        print('ERROR: Number of nodes in <dict type="map"> must be even')
        sys.exit(-1)
      count = count >> 1
      if (count > 0):
        parse_elem(elem[0], child_tabs)
        parse_elem(elem[1], child_tabs)
      if (count > 1):
        print(child_tabs, '(skipping ', count - 1, ' item', '' if (count - 1) == 1 else 's' , ')', sep='')
      print(tabs, '</', elem.tag, '>', sep='')
    else:
      print(tabs, '<', elem.tag, '>', sep='')
      for child in elem:
        parse_elem(child, child_tabs)
      print(tabs, '</', elem.tag, '>', sep='')

root = ET.parse('template.plist').getroot()

parse_elem(root, '')
