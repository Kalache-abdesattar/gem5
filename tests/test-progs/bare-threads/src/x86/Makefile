
../../bin/x86/threads: threads.cpp
	@mkdir -p ../../bin/x86/
	@g++ -static -o ../../bin/x86/threads threads.cpp -pthread -std=c++11

run: ../../bin/x86/threads
	@./../../bin/x86/threads

dump: ../../bin/x86/threads
	@mkdir -p ../../dump 
	@objdump -D -Mintel -S ../bin/x86/threads > ../../dump/threads.asm

clean:
	@rm -rf ../../bin/x86/
	@rm -rf ../../dump/