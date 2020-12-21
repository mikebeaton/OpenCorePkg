#  Copyright (c) 2020, Mike Beaton. All rights reserved.
#  SPDX-License-Identifier: BSD-3-Clause

# Basic multiple grouped targets below don't work with macOS default make (GNU Make 3.81),
# use e.g. gmake (currently GNU Make 4.3) from `brew install make` instead.

targets := mapped.plist test.c test.h
debug_targets := _test.c _test.h _OcConfigurationLib.c _OcConfigurationLib.h
live_targets := OcConfigurationLib.c ../../Include/Acidanthera/Library/OcConfigurationLib.h ../../Docs/SampleCustom.plist

.PHONY: all
all: $(targets)

.PHONY: debug
debug: $(debug_targets)

.PHONY: clean
clean:
	rm -f $(targets) $(debug_targets)

.PHONY: live
live: $(live_targets)

$(targets) &: PlistToConfig.py Template.plist
	./PlistToConfig.py -f 0x1f Template.plist -c test.c -h test.h > mapped.plist

$(live_targets) &: PlistToConfig.py Template.plist
	./PlistToConfig.py -f 0x21 Template.plist -c OcConfigurationLib.c -h ../../Include/Acidanthera/Library/OcConfigurationLib.h > ../../Docs/SampleCustom.plist

_%.c: %.c
	./StripSpace.py $^ > $@

vpath %.h ../../Include/Acidanthera/Library

_%.h: %.h
	./StripSpace.py $^ > $@
