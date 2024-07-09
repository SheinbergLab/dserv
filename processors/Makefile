CC = gcc
CFLAGS = -I.. -I. -I ../src

all: up_down_left_right.so in_out.so windows.so touch_windows.so

in_out.so: in_out.c prmutil.c ../src/dpoint_process.h
	$(CC) $(CFLAGS) -shared  in_out.c prmutil.c -o  in_out.so

up_down_left_right.so: up_down_left_right.c prmutil.c ../src/dpoint_process.h
	$(CC) $(CFLAGS)  -shared  up_down_left_right.c prmutil.c -o  up_down_left_right.so

windows.so: windows.c prmutil.c ../src/dpoint_process.h
	$(CC) $(CFLAGS) -shared  windows.c prmutil.c -o  windows.so

touch_windows.so: touch_windows.c prmutil.c ../src/dpoint_process.h
	$(CC) $(CFLAGS) -shared  touch_windows.c prmutil.c -o touch_windows.so
