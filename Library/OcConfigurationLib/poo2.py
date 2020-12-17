#!/usr/bin/env python3

#  Copyright (c) 2020, Mike Beaton. All rights reserved.
#  SPDX-License-Identifier: BSD-3-Clause

"""
Generate OpenCore .c and .h plist config definition files from template plist file.
"""

import io
import sys
import xml.etree.ElementTree as et

# Available flags for -f:

# show processed template as we have understood it, use with SHOW_ORIGINAL to recreate original plist
SHOW_XML = 0x01
# show processing steps
SHOW_DEBUG = 0x08
# use with SHOW_PLIST to recreate original plist (i.e. with extra features and hidden elements removed)
SHOW_ORIGINAL = 0x20

flags = 0x01

# emit comment in into .h file
def emit_comment(comment):
  pass

# hold .h and .c nodes
class hc:
  def __init__(self, h, c = None):
    self.h = h
    self.c = c

# plist key
class plist_key:
  def __init__(self, value, node, remove = None):
    if node is None:
      node = hc(None, None)

    if value is not None:
      if node.h is None:
        node.h = value.upper()
      if node.c is None:
        node.c = value

    self.value = value
    self.node = node
    self.remove = remove

# oc type
class oc_type:
  def __init__(self, schema_type, default = None, data_type = None, data_size = None, of = None, ref = None):
    if ref is None:
      ref = hc(None)
    self.schema_type = schema_type
    self.default = default
    self.data_type = data_type
    self.data_size = data_size
    self.of = of
    self.ref = ref

# error and stop
def error(*args, **kwargs):
  print('ERROR: ', *args, sep='', file=sys.stderr, **kwargs)
  sys.exit(-1)

# internal error and stop
def internal_error(*args, **kwargs):
  print('INTERNAL_ERROR: ', *args, sep='', file=sys.stderr, **kwargs)
  sys.exit(-1)

# debug print
def debug(*args, **kwargs):
  if flags & SHOW_DEBUG != 0:
    print('DEBUG: ', *args, sep='', **kwargs)

# print with tab stops
def info_print(*args, **kwargs):
  if kwargs.pop('info_flags', 0) & flags != 0:
    for _ in range(0, kwargs.pop('tab', 0)):
      print(end='\t')
    print(*args, sep='', **kwargs)

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

# print closing XML tag
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

# plist key
def parse_key(elem, tab, use_value = True):
  start_tag(elem, 'key', tab)
  node_h = consume_attr(elem, 'node_h')
  node_c = consume_attr(elem, 'node_c')
  section = consume_attr(elem, 'section')
  remove = consume_attr(elem, 'remove')
  if section is not None and tab != 1:
    error('section attr in tag <key> only expected at level 1 nesting')
  end_and_close_tag(elem)
  node = hc(node_h, node_c)
  return plist_key(elem.text if use_value else None, node, remove)

# apply key to attach additional info to value
def apply_key(key, value):
  pass

# data or integer
def parse_data(elem, tab, is_integer = False):
  data_size = consume_attr(elem, 'size')
  data_type = consume_attr(elem, 'type')
  default = consume_attr(elem, 'default')
  return oc_type('integer' if is_integer else 'data', default = default, data_type = data_type, data_size = data_size)

# boolean
def parse_boolean(elem, tab, value):
  default = consume_attr(elem, 'default')
  return oc_type('boolean', default = default)

# boolean
def parse_string(elem, tab):
  default = consume_attr(elem, 'default')
  return oc_type('string', default = default)

# pointer (artificial tag to add elements to data definition, cannot be populated from genuine .plist)
def parse_pointer(elem, tab):
  data_type = consume_attr(elem, 'type')
  return oc_type('pointer', data_type = data_type)

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
       
  end_and_close_tag(elem)

  return retval

# plist array
def parse_array(elem, tab):
  start_tag(elem, 'array', tab)
  singular = consume_attr(elem, 'singular')
  node_h = consume_attr(elem, 'node_h')
  node_c = consume_attr(elem, 'node_c')
  comment = consume_attr(elem, 'comment')
  xref = consume_attr(elem, 'xref')
  consume_attr(elem, 'hide') ###
  end_tag(elem)

  if comment is not None:
    emit_comment(comment)

  node = hc(node_h, node_c)
  key = plist_key(singular, node)

  value = parse_type_no_array(elem[0], tab + 1)

  close_tag(elem, tab)

  apply_key(key, value)

  return oc_type('array', of = value, ref = hc(xref))

# use when type can include array (i.e. everywhere except for inside an array)
def parse_type_or_array(key, elem, tab):
  if elem.tag == 'array':
    return parse_array(elem, tab)
  else:
    return parse_type_no_array(elem, tab)

# parse contents of dict as map
def parse_map(elem, tab):
  key = parse_key(elem[0], tab + 1, False)
  value = parse_type_or_array(key, elem[1], tab + 1)
  return value

# parse contents of dict as struct
def parse_struct(elem, tab):
  count = len(elem)
  fields = []
  index = 0
  while index < count:
    key = parse_key(elem[index], tab + 1)
    index += 1

    value = parse_type_or_array(key, elem[index], tab + 1)
    index += 1

    fields.append(value)

  return oc_type('struct', of = fields)

# parse dict (with sub-types struct and map)
def parse_dict(elem, tab):
  start_tag(elem, 'dict', tab)
  dict_type = consume_attr(elem, 'type')
  comment = consume_attr(elem, 'comment')
  consume_attr(elem, 'hide') ###
  xref = consume_attr(elem, 'xref')
  end_tag(elem)

  if comment is not None:
    emit_comment(comment)

  if dict_type == 'map':
    retval = parse_map(elem, tab)
    if xref is not None:
      error('attr xref not supported on <dict type="map">')
  elif dict_type is not None:
    error('unknown value of attr type="%s" in <dict>' % dict_type)
  else:
    retval = parse_struct(elem, tab)
    if xref is not None:
      retval.ref.h = xref

  close_tag(elem, tab)

  return retval

# use where type can be either array or basic type (i.e. everywhere except for the type in an array)
def parse_type_no_array(elem, tab):
  if elem.tag == 'dict':
    return parse_dict(elem, tab)
  else:
    return parse_basic_type(elem, tab)

# parse root element
def parse_plist(elem):
  start_tag(elem, 'plist', 0)
  consume_attr(elem, 'version')
  end_tag(elem)

  parse_dict(elem[0], 0)

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
