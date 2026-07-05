all : build

TARGET:=m05_usb
TARGET_MCU:=CH32V305
TARGET_MCU_PACKAGE:=CH32V305RBT6

include ../ch32fun/ch32fun/ch32fun.mk

build : $(TARGET).bin
flash : cv_flash
clean : cv_clean
