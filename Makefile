PLATFORM ?= nrf52

GLOBALDEPS := Makefile

O ?= build.${PLATFORM}

#
# Include platform- (which in turn include CPU-) specific things
#
P := src/platform/${PLATFORM}
include ${P}/platform.mk


CFLAGS += -g3 -Os -nostdinc -Wall -fno-builtin
CFLAGS += -I. -I${P} -Isrc/platform -I${C} -Isrc/cpu -Iinclude -Isrc

LDFLAGS += -nostartfiles -nodefaultlibs

#
# Core
#

SRCS += src/init.c \
	src/main.c \
	src/task.c \
	src/libc.c \

SRCS += src/heap_simple.c

SRCS += ext/tinyprintf/tinyprintf.c

OBJS +=  ${SRCS:%.c=${O}/%.o}
OBJS :=  ${OBJS:%.s=${O}/%.o}
DEPS +=  ${OBJS:%.o=%.d}



${O}/build.elf: ${OBJS} ${GLOBALDEPS} ${LDSCRIPT}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc ${LDFLAGS} -Wl,-T${LDSCRIPT} ${OBJS} -o $@

${O}/%.o: %.c ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -MD -MP ${CFLAGS} -c $< -o $@

${O}/%.o: %.s ${GLOBALDEPS}
	@mkdir -p $(dir $@)
	${TOOLCHAIN}gcc -MD -MP ${CFLAGS} -c $< -o $@

clean:
	rm -rf "${O}"


${O}/build.bin: ${O}/build.elf
	${TOOLCHAIN}objcopy -O binary $< $@

bin: ${O}/build.bin

run: build/build.elf
	qemu-system-arm -nographic -serial mon:stdio -machine lm3s811evb -cpu cortex-m4 -kernel $@

-include ${DEPS}
