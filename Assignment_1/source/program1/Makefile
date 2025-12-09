CC := gcc
CFLAGS := -Wall -Wextra -O2

TARGET := program1
MAIN_SRC := program1.c

TEST_SRCS := \
  normal.c abort.c alarm.c bus.c floating.c hangup.c illegal_instr.c \
  interrupt.c kill.c pipe.c quit.c segment_fault.c stop.c terminate.c trap.c

TEST_BINS := $(TEST_SRCS:.c=)

.PHONY: all clean tests run-normal run-abort run-stop

all: $(TARGET) tests

$(TARGET): $(MAIN_SRC)
	$(CC) $(CFLAGS) -o $@ $^

tests: $(TEST_BINS)

$(TEST_BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) $(TEST_BINS)
