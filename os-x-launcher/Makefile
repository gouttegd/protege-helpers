MIN_OSX_VERSION_arm64 = 12.1
MIN_OSX_VERSION_x86_64 = 10.15
CC = clang -c
LD = ld

JavaAppLauncher: launcher-x86_64 launcher-arm64
	lipo -create -output $@ launcher-x86_64 launcher-arm64

launcher-%.o: launcher.c
	$(CC) -arch $* -mmacosx-version-min=$(MIN_OSX_VERSION_$*) -o $@ $<

launcher-%: launcher-%.o
	$(LD) -arch $* -macos_version_min $(MIN_OSX_VERSION_$*) \
		-L/Library/Developer/CommandLineTools/SDKs/MacOSX$(MIN_OSX_VERSION_$*).sdk/usr/lib \
		-lSystem -o $@ $<

.PHONY: clean
clean:
	rm -f JavaAppLauncher launcher-x86_64 launcher-x86_64.o launcher-arm64 launcher-arm64.o
