#
# Makefile for the linux pmfs-filesystem routines.
#

obj-$(CONFIG_PMFS) += pmfs.o
obj-$(CONFIG_PMFS_TEST_MODULE) += pmfs_test.o

pmfs-y := bbuild.o balloc.o dir.o file.o inode.o namei.o super.o symlink.o ioctl.o journal.o

pmfs-$(CONFIG_PMFS_WRITE_PROTECT) += wprotect.o
pmfs-$(CONFIG_PMFS_XIP) += xip.o
