PTOP                     = $(shell pwd)
VALID_TARGETS            = zedboard \
                           zybo
CLEAN_TARGETS            = $(VALID_TARGETS) \
				.Xil \
				Packages \
				*.layout \
				*.debug \
				*.mif \
				*.btree \
				*.log \
				*.jou \
				*.zip \
				*.str \
				*.txt
CLEAN_BDS                = $(VALID_TARGETS)
TCL_PATH                 = $(PTOP)/src/tcl
OUTPUT_PATH              = $(PTOP)/src/outputFiles
VIVADO_CMD               = vivado -mode batch -source
VIVADO_REQI_VERSION      = 2018.2
VIVADO_REQI_VERSION_STR  = v2018.2
VIVADO_HOST_VERSION_STR  = $(shell vivado -version | awk '{print $$2}' | head -n 1)
VIVADO_DEF_BASE_PATH     = /opt/Xilinx/Vivado/$(VIVADO_REQI_VERSION)
VIVADO_BASE_PATH         ?= $(VIVADO_DEF_BASE_PATH)
VIVADO_CABLE_DRIVER_PATH = $(VIVADO_BASE_PATH)/data/xicom/cable_drivers/lin64/install_script/install_drivers
BITSTREAM_TCL            = $(TCL_PATH)/build_bitstream.tcl

all: help

checkVersion:
ifneq ($(VIVADO_HOST_VERSION_STR),$(VIVADO_REQI_VERSION_STR))
	@echo -e "Vivado version $(VIVADO_HOST_VERSION_STR) detected\n\tRequires $(VIVADO_REQI_VERSION)..."
	exit 1;
endif

checkVivadoPath:
ifeq ($(wildcard $(VIVADO_BASE_PATH)/bin/vivado),)
	@echo -e "Vivado binary not found in $(VIVADO_BASE_PATH); check path..."
	exit 1;
endif

checkValidProjName:
ifeq ($(filter $(TARGET),$(VALID_TARGETS)),)
	$(info $(TARGET) does not exist...)
	exit 1;
endif

build: checkValidProjName checkVersion
	$(VIVADO_CMD) $(TCL_PATH)/$(TARGET).tcl >> $(TARGET).log
	@echo -e "Success! Open project with :"
	@echo -e "\tvivado ./$(TARGET)/$(TARGET).xpr"
	@echo -e "\n\n!!! GENERATING BITSTREAM !!!\n\n"
	$(MAKE) bitstream

bitstream: checkValidProjName checkVersion
ifeq ($(wildcard $(TARGET)),)
	$(MAKE) build
endif
	$(VIVADO_CMD) $(BITSTREAM_TCL) -tclargs $(TARGET) $(OUTPUT_PATH)
	cp $(TARGET)/$(TARGET).runs/impl_1/*.sysdef $(OUTPUT_PATH)/$(TARGET).hdf
	cp $(TARGET)/$(TARGET).runs/impl_1/*.bit $(OUTPUT_PATH)/$(TARGET).bit

clean-all: $(CLEAN_BDS)
	rm -rf $(CLEAN_TARGETS)

$(CLEAN_BDS):
	rm -rf ./src/bds/$@/ip
	rm -rf ./src/bds/$@/ui
	rm -rf ./src/bds/$@/hdl
	rm -rf ./src/bds/$@/hw_handoff
	rm -rf ./src/bds/$@/synth
	rm -rf ./src/bds/$@/sim
	rm -rf ./src/bds/$@/ipshared
	rm -rf ./src/bds/$@/*.bxml
	rm -rf ./src/bds/$@/*.xdc

help:
	@echo -e "Requires Vivado $(VIVADO_REQI_VERSION_STR)... $(VIVADO_HOST_VERSION_STR) found in PATH...\n"
	@echo -e "make build TARGET=projName"
	@echo -e "\tBuilds vivado project, valid targets : $(VALID_TARGETS)\n"
	@echo -e "make clean-all"
	@echo -e "\tTo remove all auto-generated files\n"

.PHONY: clean-all help
