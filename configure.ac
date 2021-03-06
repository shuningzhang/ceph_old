#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([Ceph distributed file system], [.1], 
	      [Sage Weil <sage@newdream.net>],
	      [ceph])
AM_INIT_AUTOMAKE(ceph, 0.1)

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_RANLIB
AC_PROG_MAKE_SET


# Checks for libraries.
AC_CHECK_LIB([m], [pow])
AC_CHECK_LIB([pthread], [pthread_create])

# fuse?
AC_ARG_WITH([fuse],
            [AS_HELP_STRING([--with-fuse], [use FUSE library for client])],
            [],
            [with_fuse=check])
LIBFUSE=
AS_IF([test "x$with_fuse" != xno],
	    [AC_CHECK_LIB([fuse], [fuse_main],
             [AC_SUBST([LIBFUSE], ["-lfuse"])
               AC_DEFINE([HAVE_LIBFUSE], [1],
                         [Define if you have fuse])
               HAVE_LIBFUSE=1
              ],
             [if test "x$with_fuse" != xcheck; then
                 AC_MSG_FAILURE(
                   [--with-fuse was given, but test failed])
               fi
             ])])
AM_CONDITIONAL(WITH_FUSE, [test "$HAVE_LIBFUSE" = "1"])

# CommonC++?
AC_ARG_WITH([ccgnu],
            [AS_HELP_STRING([--with-ccgnu],
              [use CommonC++ library for fast thread-safe reference counting])],
            [],
            [with_ccgnu=check])
LIBCCGNU2=
AS_IF([test "x$with_ccgnu" != xno],
	    [AC_CHECK_LIB([ccgnu2], [main],
             [AC_SUBST([LIBCCGNU2], ["-lccgnu2"])
              AC_DEFINE([WITH_CCGNU], [1],
                        [Define if you have ccgnu])
	      LIBS="-lccgnu2 $LIBS"
             ],
             [if test "x$with_ccgnu" != xcheck; then
                 AC_MSG_FAILURE(
                   [--with-ccgnu was given, but test failed])
               fi
             ], [-ldl -lpthread])])
AM_CONDITIONAL(WITH_CCGNU, test "WITH_CCGNU" = "1")

# newsyn?  requires mpi.
#AC_ARG_WITH([newsyn],
#            [AS_HELP_STRING([--with-newsyn], [build newsyn target, requires mpi])],
#            [],
#            [with_newsyn=no])

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netdb.h netinet/in.h stdint.h stdlib.h string.h sys/file.h sys/ioctl.h sys/mount.h sys/param.h sys/socket.h sys/statvfs.h sys/time.h sys/vfs.h syslog.h unistd.h utime.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_CONST
AC_TYPE_UID_T
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_INT8_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_blksize])
AC_STRUCT_ST_BLOCKS
AC_CHECK_MEMBERS([struct stat.st_rdev])
AC_HEADER_TIME
AC_STRUCT_TM
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_CHOWN
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_PROG_GCC_TRADITIONAL
AC_FUNC_LSTAT
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_FUNC_SELECT_ARGTYPES
AC_TYPE_SIGNAL
AC_FUNC_STAT
AC_FUNC_UTIME_NULL
AC_CHECK_FUNCS([bzero fchdir fdatasync floor ftruncate getcwd gethostbyname gethostname gettimeofday inet_ntoa localtime_r memmove memset mkdir munmap pow rmdir select socket sqrt strcasecmp strchr strerror strstr utime])

AC_CONFIG_HEADERS([src/acconfig.h])
AC_CONFIG_FILES([Makefile
	src/Makefile])
AC_OUTPUT

AM_INIT_AUTOMAKE
