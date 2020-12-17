#!/usr/bin/env python3

#  Copyright (c) 2020, Mike Beaton. All rights reserved.
#  SPDX-License-Identifier: BSD-3-Clause

"""
Generate OpenCore .c and .h plist config definition files from template plist file.
"""

import io
import sys
import xml.etree.ElementTree as et

# dataclasses print nicely
from dataclasses import dataclass

# Available flags for -f:

# show processed template as we have understood it (use with SHOW_ORIGINAL to recreate original plist)
SHOW_XML = 0x01
# show creation of key objects
SHOW_KEYS = 0x02
# show creation of OC type objects
SHOW_OC_TYPES = 0x04
# show processing steps
SHOW_DEBUG = 0x08
# use with SHOW_PLIST to recreate original plist (i.e. with extra features and hidden elements removed)
SHOW_ORIGINAL = 0x20

flags = 0xf

# emit comment in into .h file
def emit_comment(comment):
  pass

# hold .h and .c nodes
@dataclass
class hc:
  h: str
  c: str

  def __init__(self, h=None, c=None):
    self.h = h
    self.c = c

# plist key
class plist_key:
  def __init__(self, origin, value, node, tab, remove = None):
    if node is None:
      node = hc(None, None)

    if value is not None:
      if node.h is None:
        node.h = value.upper()
      if node.c is None:
        node.c = value

    self.origin = origin
    self.value = value
    self.node = node
    self.remove = remove

    plist_key_print('[plist:key', tab=tab, end='')
    if origin != 'key':
      plist_key_attr_print('from', origin)
    plist_key_attr_print('value', value)
    plist_key_attr_print('node', node)
    plist_key_attr_print('remove', remove)
    plist_key_print(']')

# oc type
class oc_type:
  def __init__(self, schema_type, tab, default = None, path = None, size = None, of = None, ref = None):
    if ref is None:
      ref = hc()
    self.name = None
    self.schema_type = schema_type
    self.default = default
    self.path = path
    self.size = size
    self.of = of
    self.ref = ref
    self.remove = None

    oc_type_print('[oc:%s' % schema_type, tab=tab, end='')
    oc_type_attr_print('default', default)
    oc_type_attr_print('path', path)
    oc_type_attr_print('size', size)
    of_type = None
    if of is not None:
      if type(of) is list:
        of_type = 'list[%d]' % len(of)
      else:
        of_type = of.schema_type
    oc_type_attr_print('of', of_type)
    oc_type_attr_print('ref', ref)
    oc_type_print(']')

# error and stop
def error(*args, **kwargs):
  print('ERROR:', *args, file=sys.stderr, **kwargs)
  sys.exit(-1)

# internal error and stop
def internal_error(*args, **kwargs):
  print('INTERNAL_ERROR:', *args, file=sys.stderr, **kwargs)
  sys.exit(-1)

# debug print
def debug(*args, **kwargs):
  if flags & SHOW_DEBUG != 0:
    print('DEBUG:', *args, **kwargs)

# print with tab stops
def info_print(*args, **kwargs):
  if kwargs.pop('info_flags', 0) & flags != 0:
    for _ in range(0, kwargs.pop('tab', 0)):
      print(end='\t')
    print(*args, **kwargs)

# display generated objects

def plist_key_print(*args, **kwargs):
  info_print(*args, info_flags=SHOW_KEYS, **kwargs)

def oc_type_print(*args, **kwargs):
  info_print(*args, info_flags=SHOW_OC_TYPES, **kwargs)

def attr_print(name, value, flags):
  if value is not None:
    info_print(' %s=' % name, info_flags=flags, end='')
    if type(value) is list:
      info_print(value, info_flags=flags, end='')
    elif type(value) is str:
      info_print('"%s"' % value, info_flags=flags, end='')
    else:
      info_print(value, info_flags=flags, end='')

def plist_key_attr_print(name, value):
  attr_print(name, value, SHOW_KEYS)

def oc_type_attr_print(name, value):
  attr_print(name, value, SHOW_OC_TYPES)

# print part of XML output (i.e. processed plist as we have understood it)
def xml_print(*args, **kwargs):
  info_print(*args, info_flags=SHOW_XML, **kwargs)

# start of opening XML tag
def start_tag(elem, name, tab):
  if elem.tag != name:
    error('expected <%s> but found <%s>' % (name, elem.tag))
  xml_print('<%s' % name, tab = tab, end = '')

# end of opening XML tag
def end_tag(elem):
  xml_print('>')
  if len(elem.attrib) > 0:
    error('unhandled attributes %s in tag <%s>' % (elem.attrib, elem.tag))

# end and close one-line XML tag
def end_and_close_tag(elem):
  if elem.text is None:
    xml_print('/', end = '')
  else:
    xml_print('>%s</%s' % (elem.text, elem.tag), end = '')
  end_tag(elem) # abusing 'end' slightly, if we're ending the closing tag

# closing XML tag
def close_tag(elem, tab):
  xml_print('</%s>' % elem.tag, tab = tab)

# display XML attr
def display_attr(name, value):
  xml_print(' %s="%s"' % (name, value), end = '')

# consume (and optionally display as we go) XML attr
def consume_attr(elem, name, display = True):
  value = elem.attrib.pop(name, None)
  if display and value is not None:
    display_attr(name, value)
  return value

# convert sensible XML or HTML values to Python True or False
def bool_from_str(str_bool):
  if str_bool is None:
    bool_bool = None
  else:
    lower = str_bool.lower()
    if str_bool == "0" or lower == "false" or lower == "no":
      bool_bool = False
    elif str_bool == "1" or lower == "true" or lower == "yes":
      bool_bool = True
    else:
      error('illegal bool value="', str_bool, '"')
  return bool_bool

# plist key; option not to use value for is for keys in maps
def parse_key(elem, tab, use_value = True):
  start_tag(elem, 'key', tab)
  h = consume_attr(elem, 'h')
  c = consume_attr(elem, 'c')
  section = consume_attr(elem, 'section')
  remove = bool_from_str(consume_attr(elem, 'remove'))
  end_and_close_tag(elem)

  if section is not None and tab != 1:
    error('section attr in tag <key> only expected at level 1 nesting')

  return plist_key('key', elem.text if use_value else None, hc(h, c), tab, remove)

# apply key to attach additional info to value
def apply_key(key, oc, tab):
  if oc.name is not None:
    internal_error('name should not get set more than once on oc_type')

  oc.name = key.value
  oc_type_print('[oc: ... name="%s"' % oc.name, tab = tab + 1, end='')

  oc.remove = key.remove
  if oc.remove is not None:
    oc_type_print(' remove=%s' % oc.remove, end='')

  oc_type_print(']')

# data or integer
def parse_data(elem, tab, is_integer = False):
  data_size = consume_attr(elem, 'size')
  data_type = consume_attr(elem, 'type')
  default = consume_attr(elem, 'default')
  end_and_close_tag(elem)
  return oc_type('integer' if is_integer else 'data', tab, default = default, size = data_size, ref=hc(data_type))

# boolean
def parse_boolean(elem, tab, value):
  default = consume_attr(elem, 'default')
  end_and_close_tag(elem)
  return oc_type('boolean', tab, default = default)

# boolean
def parse_string(elem, tab):
  default = consume_attr(elem, 'default')
  end_and_close_tag(elem)
  return oc_type('string', tab, default = default)

# pointer (artificial tag to add elements to data definition, cannot be populated from genuine .plist)
def parse_pointer(elem, tab):
  data_type = consume_attr(elem, 'type')
  end_and_close_tag(elem)
  return oc_type('pointer', tab, ref=hc(data_type))

# plist basic type
def parse_basic_type(elem, tab):
  start_tag(elem, elem.tag, tab)

  if elem.tag == 'data':
    retval = parse_data(elem, tab)
  elif elem.tag == 'integer':
    retval = parse_data(elem, tab, True)
  elif elem.tag == 'string':
    retval = parse_string(elem, tab)
  elif elem.tag == 'false':
    retval = parse_boolean(elem, tab, False)
  elif elem.tag == 'true':
    retval = parse_boolean(elem, tab, True)
  elif elem.tag == 'pointer':
    retval = parse_pointer(elem, tab)
  else:
    error('unexpected tag for basic type, found <%s>' % elem.tag)

  return retval

# plist array
def parse_array(elem, path, tab):
  start_tag(elem, 'array', tab)
  singular = consume_attr(elem, 'singular')
  h = consume_attr(elem, 'h')
  c = consume_attr(elem, 'c')
  comment = consume_attr(elem, 'comment')
  xref = consume_attr(elem, 'xref')
  consume_attr(elem, 'hide') ###
  end_tag(elem)

  if comment is not None:
    emit_comment(comment)

  if singular is not None or c is not None or h is not None:
    if singular is None:
      singular = c
    node = hc(h, c)
    key = plist_key('array', singular, node, tab)
    new_path = path.copy()
    del new_path[-1]
    new_path.append(key.node)
  else:
    new_path = path
    key = None

  value = parse_type_no_array(elem[0], new_path, tab + 1)

  close_tag(elem, tab)

  return oc_type('array', tab, path = path, of = value, ref = hc(xref))

# use when type can include array (i.e. everywhere except for inside an array)
def parse_type_or_array(elem, path, tab):
  if elem.tag == 'array':
    return parse_array(elem, path, tab)
  else:
    return parse_type_no_array(elem, path, tab)

# parse contents of dict as map
def parse_map(elem, path, tab):
  index = 0

  parse_key(elem[index], tab + 1, False) ###
  index += 1

  value = parse_type_or_array(elem[1], path, tab + 1)
  index += 1

  close_tag(elem, tab)

  return oc_type('map', tab, of = value)


# parse contents of dict as struct
def parse_struct(elem, path, tab):
  fields = []

  index = 0
  count = len(elem)

  while index < count:
    key = parse_key(elem[index], tab + 1)
    index += 1

    new_path = path.copy()
    new_path.append(key.node)

    value = parse_type_or_array(elem[index], new_path, tab + 1)
    index += 1

    apply_key(key, value, tab)

    fields.append(value)

  close_tag(elem, tab)

  return oc_type('struct', tab, path = path, of = fields)

# parse dict (with sub-types struct and map)
def parse_dict(elem, path, tab):
  start_tag(elem, 'dict', tab)
  dict_type = consume_attr(elem, 'type')
  comment = consume_attr(elem, 'comment')
  consume_attr(elem, 'hide') ###
  xref = consume_attr(elem, 'xref')
  end_tag(elem)

  if comment is not None:
    emit_comment(comment)

  if dict_type == 'map':
    retval = parse_map(elem, path, tab)
    if xref is not None:
      error('attr xref not supported on <dict type="map">')
  elif dict_type is not None:
    error('unknown value of attr type="%s" in <dict>' % dict_type)
  else:
    retval = parse_struct(elem, path, tab)
    if xref is not None:
      retval.ref.h = xref

  return retval

# use where type can be either array or basic type (i.e. everywhere except for the type in an array)
def parse_type_no_array(elem, path, tab):
  if elem.tag == 'dict':
    return parse_dict(elem, path, tab)
  else:
    return parse_basic_type(elem, tab)

# parse root element
def parse_plist(elem):
  start_tag(elem, 'plist', 0)
  consume_attr(elem, 'version')
  end_tag(elem)

  parse_dict(elem[0], [], 0)

  close_tag(elem, 0)

# main()
def main():
  if len(sys.argv) < 2:
    print('provide plist filename to parse')
    return

  plist = et.parse(sys.argv[1]).getroot()

  parse_plist(plist)

# go
main()
sys.exit(0)
