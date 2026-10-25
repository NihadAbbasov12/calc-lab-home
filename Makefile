# The exact build command used for grading
calc: calc.c
	gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm

# The build that must also be clean for grade 10
asan: calc.c
	gcc -O1 -g -Wall -Wextra -std=c17 -fsanitize=address,undefined -o calc_asan calc.c -lm

test: calc
	sh calc-tests/run_tests.sh ./calc

cli: calc
	sh calc-tests/cli.sh ./calc

clean:
	rm -f calc calc_asan

.PHONY: asan test cli clean
