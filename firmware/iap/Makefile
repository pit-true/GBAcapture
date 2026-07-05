all : build

TARGET:=iap
TARGET_MCU:=CH32V305
TARGET_MCU_PACKAGE:=CH32V305RBT6
LINKER_SCRIPT:=iap_16k.ld

include ../ch32fun/ch32fun/ch32fun.mk

build : $(TARGET).bin
clean : cv_clean
