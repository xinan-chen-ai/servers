KERNEL_DIR=/home/jason/share/Android11/android11-dev/kernel

ARCH=arm64

CROSS_COMPILE=aarch64-linux-gnu-

export  ARCH  CROSS_COMPILE

obj-m := timer.o

all:
        $(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

.PHONE:clean

clean:
        $(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
