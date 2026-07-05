all : build

TARGET:=gbacap_app
TARGET_MCU:=CH32V305
TARGET_MCU_PACKAGE:=CH32V305RBT6
# APP は IAP (16KB) の後ろ 0x08004000 に配置される (docs/flashing.md)
LINKER_SCRIPT:=app_offset.ld

include ../ch32fun/ch32fun/ch32fun.mk

build : $(TARGET).bin
flash : cv_flash
clean : cv_clean
