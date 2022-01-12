NAME	= vsfs
KDIR	= /lib/modules/$(shell uname -r)/build
PWD	= $(shell pwd)

obj-m		+= $(NAME).o

$(NAME)-y	:= super.o inode.o dir.o namei.o

all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean 
