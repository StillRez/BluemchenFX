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
DAISY   := $(ROOT)/lib/libDaisy/src
BUILD   := build

CXX      ?= g++
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wno-unused-function -MMD -MP \
            -DUSE_DAISYSP_LGPL \
            -DSD_DIR='"$(abspath $(ROOT)/sd-card/BLUEMCHN)"' \
            -Istubs -I$(SRC) -I$(DSP) -I$(DSPLGPL) -I$(DAISY) \
            $(addprefix -I,$(wildcard $(DSP)/*/ $(DSPLGPL)/*/))

# Firmware sources under test
FW_SOURCES := \
	$(SRC)/fx/reverb.cpp \
	$(SRC)/fx/delay.cpp \
	$(SRC)/fx/registry.cpp \
	$(SRC)/app/chain.cpp \
	$(SRC)/app/chain_settings.cpp \
	$(SRC)/app/preset_text.cpp \
	$(SRC)/ui/ui.cpp

TEST_SOURCES := $(wildcard *.cpp)
DSP_SOURCES  := $(wildcard $(DSP)/*/*.cpp) $(wildcard $(DSPLGPL)/*/*.cpp)
# libDaisy's display fonts (the UI tests draw into a framebuffer)
C_SOURCES    := $(DAISY)/util/oled_fonts.c

obj = $(BUILD)/$(subst ..,up,$(1:.cpp=.o))
cobj = $(BUILD)/$(subst ..,up,$(1:.c=.o))
OBJS := $(foreach s,$(TEST_SOURCES) $(FW_SOURCES) $(DSP_SOURCES),$(call obj,$(s))) \
        $(foreach s,$(C_SOURCES),$(call cobj,$(s)))

all: $(BUILD)/run_tests

$(BUILD)/run_tests: $(OBJS)
	$(CXX) -o $@ $^ -lm

define compile_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) -c $$< -o $$@
endef
$(foreach s,$(TEST_SOURCES) $(FW_SOURCES) $(DSP_SOURCES),$(eval $(call compile_rule,$(s))))

$(foreach s,$(C_SOURCES),$(eval $(call cobj,$(s)): $(s) ; @mkdir -p $$(dir $$@) && $$(CC) -O2 -I$(DAISY) -c $$< -o $$@))

run: $(BUILD)/run_tests
	./$(BUILD)/run_tests $(ARGS)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
-include $(OBJS:.o=.d)
