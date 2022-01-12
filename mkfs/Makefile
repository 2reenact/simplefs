CC = gcc
CFLAG = -I.
DEPS = vsfs_fs.h mkfs.h
OBJ = mkfs_lib.o mkfs_io.o mkfs_format.o mkfs_main.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAG)

mkfs.vsfs: $(OBJ)
	$(CC) -o $@ $^ $(CFLAG)

clean:
	rm $(OBJ) mkfs.vsfs
