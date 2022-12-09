MIN_OSX_VERSION_arm64 = 12.1
MIN_OSX_VERSION_x86_64 = 10.15
CC = clang -c
LD = ld

MACOS_SDKS_DIR = /Library/Developer/CommandLineTools/SDKs

JDKS_DIR = /Library/Java/JavaVirtualMachines
JDK_NAME = zulu-11.jdk
JDK_INCLUDE_DIR = $(JDKS_DIR)/$(JDK_NAME)/Contents/Home/include

protege: launcher-x86_64 launcher-arm64
	lipo -create -output $@ launcher-x86_64 launcher-arm64

launcher-%.o: launcher.c
	$(CC) -arch $* -mmacosx-version-min=$(MIN_OSX_VERSION_$*) \
	  -I$(JDK_INCLUDE_DIR) -I$(JDK_INCLUDE_DIR)/darwin \
	  -o $@ $<

launcher-%: launcher-%.o
	$(LD) -arch $* -macos_version_min $(MIN_OSX_VERSION_$*) \
		-L$(MACOS_SDKS_DIR)/MacOSX$(MIN_OSX_VERSION_$*).sdk/usr/lib \
		-F$(MACOS_SDKS_DIR)/MacOSX$(MIN_OSX_VERSION_$*).sdk/System/Library/Frameworks \
		-lSystem -framework Cocoa -o $@ $<

.PHONY: clean
clean:
	rm -f protege launcher-x86_64 launcher-x86_64.o launcher-arm64 launcher-arm64.o
