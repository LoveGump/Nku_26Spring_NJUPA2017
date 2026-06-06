NAME = nemu
INC_DIR += ./include
BUILD_DIR ?= ./build
OBJ_DIR ?= $(BUILD_DIR)/obj
BINARY ?= $(BUILD_DIR)/$(NAME)

include Makefile.git

.DEFAULT_GOAL = app

# Compilation flags
CC = gcc
LD = gcc
INCLUDES  = $(addprefix -I, $(INC_DIR))
CFLAGS   += -O2 -MMD -Wall -Werror -ggdb $(INCLUDES)

JIT ?= 0
ifeq ($(JIT),1)
CFLAGS += -DCONFIG_JIT
endif

# Files to be compiled
SRCS = $(shell find src/ -name "*.c")
CODE_FILES = $(shell find src/ include/ -type f \( -name "*.c" -o -name "*.h" \))
COUNT_BASE ?= pa0
COUNT_REF ?= HEAD
OBJS = $(SRCS:src/%.c=$(OBJ_DIR)/%.o)

# Compilation patterns
$(OBJ_DIR)/%.o: src/%.c
	@echo + CC $<
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

# Depencies
-include $(OBJS:.o=.d)

# Some convinient rules

.PHONY: app run gdb submit clean count count-nonblank
app: $(BINARY)

ARGS ?= -l $(BUILD_DIR)/nemu-log.txt

# Command to execute NEMU
NEMU_EXEC := $(BINARY) $(ARGS)

# Use `make count BASE_BRANCH=origin/2017` if your framework baseline is `origin/2017`.
BASE_BRANCH ?= pa1
COUNT_FILE_EXPR = find . -type f \( -name "*.c" -o -name "*.h" \)

$(BINARY): $(OBJS)
	$(call git_commit, "compile")
	@echo + LD $@
	@$(LD) -O2 -o $@ $^ -lSDL2 -lreadline

run: $(BINARY)
	$(call git_commit, "run")
	$(NEMU_EXEC)

gdb: $(BINARY)
	$(call git_commit, "gdb")
	gdb -s $(BINARY) --args $(NEMU_EXEC)

count:
	@cur_total=$$($(COUNT_FILE_EXPR) -print0 | xargs -0 cat | wc -l); \
	base_total=$$(git -C .. ls-tree -r --name-only $(BASE_BRANCH) -- nemu | grep -E '\.(c|h)$$' | while read -r f; do git -C .. show $(BASE_BRANCH):$$f; done | wc -l); \
	echo "current total lines (.c/.h): $$cur_total"; \
	echo "$(BASE_BRANCH) total lines (.c/.h): $$base_total"; \
	echo "added lines since $(BASE_BRANCH): $$((cur_total - base_total))"

count-nonblank:
	@cur_nonblank=$$($(COUNT_FILE_EXPR) -print0 | xargs -0 cat | grep -av '^[[:space:]]*$$' | wc -l); \
	base_nonblank=$$(git -C .. ls-tree -r --name-only $(BASE_BRANCH) -- nemu | grep -E '\.(c|h)$$' | while read -r f; do git -C .. show $(BASE_BRANCH):$$f; done | grep -av '^[[:space:]]*$$' | wc -l); \
	echo "current non-blank lines (.c/.h): $$cur_nonblank"; \
	echo "$(BASE_BRANCH) non-blank lines (.c/.h): $$base_nonblank"; \
	echo "added non-blank lines since $(BASE_BRANCH): $$((cur_nonblank - base_nonblank))"

clean: 
	rm -rf $(BUILD_DIR)
