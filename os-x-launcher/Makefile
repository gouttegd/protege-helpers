CC = clang

JavaAppLauncher: launcher-x86_64 launcher-arm64
	lipo -create -output $@ launcher-x86_64 launcher-arm64

launcher-x86_64: launcher.c
	arch -x86_64 clang -o $@ $<

launcher-arm64: launcher.c
	arch -arm64 clang -o $@ $<

.PHONY: clean
clean:
	rm -f JavaAppLauncher launcher-x86_64 launcher-arm64
