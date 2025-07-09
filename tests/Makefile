run_chi_custom:
	./../build/ALL/gem5.opt --debug-flags=ExecAll,MemoryAccess,RubyPort,Cache,RubyCache,RubyNetwork --debug-file=cache_trace.txt --debug-start=50000 --debug-end=1000000000 gem5/chi_custom/configs/chi-with-isa.py riscv --num-cores=4 --resource-directory=gem5/resources


run_chi_threads:
	./../build/ALL/gem5.opt gem5/chi_threads/configs/chi-with-isa.py x86 --num-cores=4 --resource-directory=test-progs/bare-threads/bin/x86/