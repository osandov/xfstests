AC_DEFUN([AC_PACKAGE_WANT_LZO],
  [ AC_CHECK_HEADERS(lzo/lzo1x.h, [ have_lzo=true ], [ have_lzo=false ])
    AC_SUBST(have_lzo)
  ])

AC_DEFUN([AC_PACKAGE_WANT_ZLIB],
  [ AC_CHECK_HEADERS(zlib.h, [ have_zlib=true ], [ have_zlib=false ])
    AC_SUBST(have_zlib)
  ])

AC_DEFUN([AC_PACKAGE_WANT_ZSTD],
  [ AC_CHECK_HEADERS(zstd.h, [ have_zstd=true ], [ have_zstd=false ])
    AC_SUBST(have_zstd)
  ])
