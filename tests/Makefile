# Host-side tests for BluemchenFX (runs on the PC, no hardware needed).
#
#   make -C tests run            build and run all tests
#   make -C tests run ARGS=delay only tests whose name contains "delay"
#
# Builds the hardware-independent firmware sources (effects, preset text
# format) plus DaisySP / DaisySP-LGPL with the native g++. libDaisy is not
# used; tests/stubs/ stands in for the few headers the effects include.

ROOT    := ..
SRC     := $(ROOT)/src
DSP     := $(ROOT)/lib/DaisySP/Source
DSPLGPL := $(ROOT)/lib/DaisySP/DaisySP-LGPL/Source
BUILD   := build

CXX      ?= g++
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wno-unused-function -MMD -MP \
            -DUSE_DAISYSP_LGPL \
            -DSD_DIR='"$(abspath $(ROOT)/sd-card/BLUEMCHN)"' \
            -Istubs -I$(SRC) -I$(DSP) -I$(DSPLGPL) \
            $(addprefix -I,$(wildcard $(DSP)/*/ $(DSPLGPL)/*/))

# Firmware sources under test
FW_SOURCES := \
	$(SRC)/fx/reverb.cpp \
	$(SRC)/fx/delay.cpp \
	$(SRC)/fx/registry.cpp \
	$(SRC)/app/chain.cpp \
	$(SRC)/app/chain_settings.cpp \
	$(SRC)/app/preset_text.cpp

TEST_SOURCES := $(wildcard *.cpp)
DSP_SOURCES  := $(wildcard $(DSP)/*/*.cpp) $(wildcard $(DSPLGPL)/*/*.cpp)

obj = $(BUILD)/$(subst ..,up,$(1:.cpp=.o))
OBJS := $(foreach s,$(TEST_SOURCES) $(FW_SOURCES) $(DSP_SOURCES),$(call obj,$(s)))

all: $(BUILD)/run_tests

$(BUILD)/run_tests: $(OBJS)
	$(CXX) -o $@ $^ -lm

define compile_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) -c $$< -o $$@
endef
$(foreach s,$(TEST_SOURCES) $(FW_SOURCES) $(DSP_SOURCES),$(eval $(call compile_rule,$(s))))

run: $(BUILD)/run_tests
	./$(BUILD)/run_tests $(ARGS)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
-include $(OBJS:.o=.d)
