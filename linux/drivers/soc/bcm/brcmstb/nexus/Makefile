obj-y	:=	irq.o \
		reg.o
obj-$(CONFIG_GPIOLIB) += gpio.o

obj-$(CONFIG_BRCMSTB_NEXUS_SPI_API) 		+= spi.o
CFLAGS_spi.o += -I$(src)/../../../../scripts/dtc/libfdt
obj-$(CONFIG_BRCMSTB_NEXUS_CLK_API) += clk.o
ifdef CONFIG_ARM_SCMI_PROTOCOL
obj-y		+= dvfs.o
endif
ifdef CONFIG_ARM_BRCMSTB_AVS_CPUFREQ
obj-y		+= dvfs.o
endif
