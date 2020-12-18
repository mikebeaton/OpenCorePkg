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

flags = 0x1

# passed around parsing to determine which files to write to
OUTPUT_PLIST = 0x1
OUTPUT_C = 0x2
OUTPUT_H = 0x4
OUTPUT_NONE = 0x0
OUTPUT_ALL = OUTPUT_H | OUTPUT_C | OUTPUT_PLIST

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
  def __init__(self, origin, value, node, tab):
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

    plist_key_print('[plist:key', tab=tab, end='')
    if origin != 'key':
      plist_key_attr_print('from', origin)
    plist_key_attr_print('value', value)
    plist_key_attr_print('node', node)

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

# print with tab stops and flags
def info_print(*args, **kwargs):
  if kwargs.pop('info_flags', 0) & flags != 0:
    for _ in range(0, kwargs.pop('tab', 0)):
      print(end='\t')
    print(*args, **kwargs)

#
# optionally display objects generated during processing
#

def plist_key_print(*args, **kwargs):
  info_print(*args, info_flags = SHOW_KEYS, **kwargs)

def oc_type_print(*args, **kwargs):
  info_print(*args, info_flags = SHOW_OC_TYPES, **kwargs)

def attr_print(name, value, flags):
  if value is not None:
    info_print(' %s=' % name, info_flags = flags, end='')
    if type(value) is list:
      info_print(value, info_flags = flags, end='')
    elif type(value) is str:
      info_print('"%s"' % value, info_flags = flags, end='')
    else:
      info_print(value, info_flags = flags, end='')

def plist_key_attr_print(name, value):
  attr_print(name, value, SHOW_KEYS)

def oc_type_attr_print(name, value):
  attr_print(name, value, SHOW_OC_TYPES)

# print part of XML output (i.e. processed plist as we have understood it)
def xml_print(*args, **kwargs):
  if kwargs.pop('out_flags', 0) & OUTPUT_PLIST != 0:
    info_print(*args, info_flags = SHOW_XML, **kwargs)

# start of opening XML tag
def start_tag(elem, name, out_flags, tab):
  if elem.tag != name:
    error('expected <%s> but found <%s>' % (name, elem.tag))
  xml_print('<%s' % name, out_flags = out_flags, tab = tab, end = '')

# end of opening XML tag
def end_tag(elem, out_flags):
  xml_print('>', out_flags = out_flags)
  if len(elem.attrib) > 0:
    error('unhandled attributes %s in tag <%s>' % (elem.attrib, elem.tag))

# end and close one-line XML tag
def end_and_close_tag(elem, out_flags, quick_close = True, kill_content = False):
  no_content = kill_content or elem.text is None
  if quick_close and no_content:
    xml_print('/', out_flags = out_flags, end = '')
  else:
    xml_print('>%s</%s' % ('' if no_content else elem.text, elem.tag), out_flags = out_flags, end = '')
  end_tag(elem, out_flags) # abusing 'end' slightly, if we're ending the closing tag

# closing XML tag
def close_tag(elem, out_flags, tab):
  xml_print('</%s>' % elem.tag, out_flags = out_flags, tab = tab)

# display XML attr
def display_attr(name, value, out_flags, displayInOriginal = False):
  if value is not None and (flags & SHOW_ORIGINAL == 0 or displayInOriginal):
    xml_print(' %s="%s"' % (name, value), out_flags = out_flags, end = '')

# consume (and optionally display as we go) XML attr
def consume_attr(elem, name, out_flags, display = True, displayInOriginal = False):
  value = elem.attrib.pop(name, None)
  if display:
    display_attr(name, value, out_flags, displayInOriginal)
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

# parse attribute controlling which files output goes to
def parse_out_attr(elem, out_flags):
  out_attr = consume_attr(elem, 'out', 0, False)

  if out_attr == None or out_attr == '':
    return (out_flags, out_attr)
  
  attr_flags = dict([(c, True) for c in out_attr.lower()])

  use_flags = out_flags

  if attr_flags.pop('c', None) is None:
    use_flags &= ~OUTPUT_C
    
  if attr_flags.pop('h', None) is None:
    use_flags &= ~OUTPUT_H
    
  if attr_flags.pop('p', None) is None:
    # only stop showing plist output if we are recreating the original plist
    if flags & SHOW_ORIGINAL != 0:
      use_flags &= ~OUTPUT_PLIST

  if len(attr_flags) > 0:
    error('unknown letter flags in attr output="%s", \'c\', \'h\' & \'p\' are allowed' % out_attr)

  return (use_flags, out_attr)

# plist key; option not to use value for is for keys in maps
def parse_key(elem, out_flags, tab, use_value = True):
  (use_flags, out_attr) = parse_out_attr(elem, out_flags)

  start_tag(elem, 'key', use_flags, tab)
  h = consume_attr(elem, 'h', use_flags)
  c = consume_attr(elem, 'c', use_flags)
  display_attr('out', out_attr, use_flags)
  section = consume_attr(elem, 'section', use_flags)
  end_and_close_tag(elem, use_flags)

  if section is not None and tab != 1:
    error('section attr in tag <key> only expected at level 1 nesting')

  key = plist_key('key', elem.text if use_value else None, hc(h, c), tab)

  return (use_flags, key)

# apply key to attach additional info to value
def apply_key(key, oc, tab):
  if oc.name is not None:
    internal_error('name should not get set more than once on oc_type')

  oc.name = key.value
  oc_type_print('[oc: ... name="%s"' % oc.name, tab = tab + 1, end='')

  oc_type_print(']')

# data or integer
def parse_data(elem, out_flags, tab, is_integer = False):
  data_size = consume_attr(elem, 'size', out_flags)
  data_type = consume_attr(elem, 'type', out_flags)
  default = consume_attr(elem, 'default', out_flags)
  end_and_close_tag(elem, out_flags, quick_close = False)
  return oc_type('integer' if is_integer else 'data', tab, default = default, size = data_size, ref=hc(data_type))

# boolean
def parse_boolean(elem, out_flags, tab, value):
  default = consume_attr(elem, 'default', out_flags)
  end_and_close_tag(elem, out_flags)
  return oc_type('boolean', tab, default = default)

# string
def parse_string(elem, out_flags, tab):
  default = consume_attr(elem, 'default', out_flags)
  end_and_close_tag(elem, out_flags, quick_close = False)
  return oc_type('string', tab, default = default)

# pointer (artificial tag to add elements to data definition, cannot be populated from genuine .plist)
def parse_pointer(elem, out_flags, tab):
  start_tag(elem, 'pointer', out_flags, tab)
  data_type = consume_attr(elem, 'type', out_flags)
  end_and_close_tag(elem, out_flags)
  return oc_type('pointer', tab, ref=hc(data_type))

# plist basic types
def parse_basic_type(elem, out_flags, tab):
  start_tag(elem, elem.tag, out_flags, tab)

  if elem.tag == 'data':
    retval = parse_data(elem, out_flags, tab)
  elif elem.tag == 'integer':
    retval = parse_data(elem, out_flags, tab, True)
  elif elem.tag == 'string':
    retval = parse_string(elem, out_flags, tab)
  elif elem.tag == 'false':
    retval = parse_boolean(elem, out_flags, tab, False)
  elif elem.tag == 'true':
    retval = parse_boolean(elem, out_flags, tab, True)
  else:
    error('unexpected tag for basic type, found <%s>' % elem.tag)

  return retval

# check for hide="children", meaning remove all child tags when outputting original plist
def hide_children(elem, out_flags):
  hide = consume_attr(elem, 'hide', out_flags)
  if hide is None:
    hiding = False
  elif hide.lower() == 'children':
    hiding = True
  else:
    error('invalid value for attr hide="%s" in <%s>' % (hide, elem.tag))

  # only hide when recreating original plist
  if flags & SHOW_ORIGINAL == 0:
    hiding = False

  if hiding:
    end_and_close_tag(elem, out_flags, kill_content = True)
    use_flags = out_flags & ~OUTPUT_PLIST
  else:
    end_tag(elem, out_flags)
    use_flags = out_flags
    
  return (hiding, use_flags)

# plist array
def parse_array(elem, path, out_flags, tab):
  start_tag(elem, 'array', out_flags, tab)
  singular = consume_attr(elem, 'singular', out_flags)
  h = consume_attr(elem, 'h', out_flags)
  c = consume_attr(elem, 'c', out_flags)
  comment = consume_attr(elem, 'comment', out_flags)
  xref = consume_attr(elem, 'xref', out_flags)

  (hiding, use_flags) = hide_children(elem, out_flags)

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

  index = 0
  value = parse_type_in_array(elem[index], new_path, use_flags, tab + 1)
  index += 1

  array_skip(elem, index, path, use_flags, tab)

  if not hiding:
    close_tag(elem, out_flags, tab)

  return oc_type('array', tab, path = path, of = value, ref = hc(xref))

# types allowed inside dict (includes array, and artificial pointer type)
def parse_type_in_dict(elem, path, out_flags, tab):
  if elem.tag == 'array':
    return parse_array(elem, path, out_flags, tab)
  elif elem.tag == 'pointer':
    return parse_pointer(elem, out_flags, tab)
  else:
    return parse_type_in_array(elem, path, out_flags, tab)

# types allowed inside array (excludes array itself, and artificial pointer type)
def parse_type_in_array(elem, path, out_flags, tab):
  if elem.tag == 'dict':
    return parse_dict(elem, path, out_flags, tab)
  else:
    return parse_basic_type(elem, out_flags, tab)

# emit message describing skip (and sort out flags describing what to do)
def skip_msg(elem, start, out_flags, tab):
  if flags & SHOW_ORIGINAL == 0:
    use_flags = OUTPUT_NONE
    count = len(elem) - start
    if count > 0:
      xml_print('(skipping %d item%s)' % (count, '' if count == 1 else 's'), out_flags = out_flags, tab = tab + 1)
  else:
    use_flags = OUTPUT_PLIST
  return use_flags

# skip unused elements
def map_skip(elem, start, path, out_flags, tab):
  use_flags = skip_msg(elem, start, out_flags, tab)
  index = start
  while index < len(elem):
    (next_flags, _) = parse_key(elem[index], use_flags, tab + 1, False)
    index += 1

    parse_type_in_dict(elem[index], path, next_flags, tab + 1)
    index += 1

# skip unused elements
def array_skip(elem, start, path, out_flags, tab):
  use_flags = skip_msg(elem, start, out_flags, tab)
  index = start
  while index < len(elem):
    parse_type_in_array(elem[index], path, use_flags, tab + 1)
    index += 1

# parse contents of dict as map
def parse_map(elem, path, hiding, out_flags, use_flags, tab):
  index = 0

  (next_flags, _) = parse_key(elem[index], use_flags, tab + 1, False)
  index += 1

  value = parse_type_in_dict(elem[1], path, next_flags, tab + 1)
  index += 1

  map_skip(elem, index, path, use_flags, tab)

  if not hiding:
    close_tag(elem, out_flags, tab)

  return oc_type('map', tab, of = value)

# parse contents of dict as struct
def parse_struct(elem, path, hiding, out_flags, use_flags, tab):
  fields = []

  index = 0
  count = len(elem)

  while index < count:
    (next_flags, key) = parse_key(elem[index], use_flags, tab + 1)
    index += 1

    new_path = path.copy()
    new_path.append(key.node)

    value = parse_type_in_dict(elem[index], new_path, next_flags, tab + 1)
    index += 1

    apply_key(key, value, tab)

    fields.append(value)

  if not hiding:
    close_tag(elem, out_flags, tab)

  return oc_type('struct', tab, path = path, of = fields)

# parse dict (with sub-types struct and map)
def parse_dict(elem, path, out_flags, tab):
  start_tag(elem, 'dict', out_flags, tab)
  dict_type = consume_attr(elem, 'type', out_flags)
  comment = consume_attr(elem, 'comment', out_flags)
  xref = consume_attr(elem, 'xref', out_flags)

  (hiding, use_flags) = hide_children(elem, out_flags)

  if comment is not None:
    emit_comment(comment)

  if dict_type == 'map':
    retval = parse_map(elem, path, hiding, out_flags, use_flags, tab)
    if xref is not None:
      error('attr xref not supported on <dict type="map">')
  elif dict_type is not None:
    error('unknown value of attr type="%s" in <dict>' % dict_type)
  else:
    retval = parse_struct(elem, path, hiding, out_flags, use_flags, tab)
    if xref is not None:
      retval.ref.h = xref

  return retval

# parse root element
def parse_plist(elem, output):
  start_tag(elem, 'plist', output, 0)
  consume_attr(elem, 'version', output, displayInOriginal = True)
  end_tag(elem, output)

  parse_dict(elem[0], [], output, 0)

  close_tag(elem, output, 0)

# if recreating original plist, emit first two lines of template file,
# which otherwise don't show up in processing
def emit_plist_header(filename):
  if flags & SHOW_ORIGINAL != 0:
    with open(filename) as plist_file:
      for _ in range(2):
        print(next(plist_file), end = '')

# main()
def main():
  if len(sys.argv) < 2:
    print('provide plist filename to parse')
    return

  plist = et.parse(sys.argv[1]).getroot()

  # do this after the above, once we know input file was a valid plist
  emit_plist_header(sys.argv[1])

  parse_plist(plist, OUTPUT_ALL)

# go
main()
sys.exit(0)
