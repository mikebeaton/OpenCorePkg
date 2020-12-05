## Root node

Root is pretty much a dict-as-fields, but every field is (currently?) a dict,
and I *think* the way the root configuration is set up in terms of these is
(though simple) unique, i.e. not quite the way that a normal dict-as-fields is
configured. (See end of OcConfigurationLib.c.)

## dict

A dict can be:

- dict-as-fields
- dict-as-map

## dict-as-fields

A dict-as-fields basically defines a C struct, fields with names and types, each possibly with initialiser. The type of each member can be t or t[]. Even for t[] there can be an initialiser value. NB This kind of array is just specified by the type having a size, and is simpler than (and not the same as) an OC_ARRAY of a type.

## dict-as-map

A dict-as-map can be:

- -&gt;OC_DATA
  - in which case the definition already exists and is called OC_ASSOC
  - think it will be better to handle this at the last minute (twice...)
- -&gt;(-&gt;OC_DATA) i.e. -&gt;OC_ASSOC
- -&gt;array
  - in which case, I suspect the only used (supported?) type in the array is string; NB not vice versa, i.e. array of string is definitely is used elsewhere, twice, as the type of a dict-as-fields member

I don't think any other target type is supported...?

In dict-as-map the key does NOT provide the field name, it is just a runtime-only key.

## Array

An array can be:

- array of dict-as-fields
- array of string

I don't think any other target type is used at the moment, but there doesn't seem to be any good reason to disallow other target types

## Basic types:

- &lt;true&gt; or &lt;false&gt;:
  - both of which are mapped to fake type &lt;bool&gt; with a value, in this code
- &lt;string&gt;
- &lt;data&gt;, which can define:
   - fixed size data
   - data blob
