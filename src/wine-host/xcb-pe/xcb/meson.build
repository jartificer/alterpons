# Including the system's include directory directly would also pull in all of
# the system's other (glibc) headers, so we'll copy only the libxcb headers we
# need to the build directory instead. `xcb.h` includes `xcb_windefs.h` when
# compiling for Windows, so we provide our own minimal version of that header.
xcb_system_include_dir = xcb_headers_dep.get_variable(pkgconfig : 'includedir')
foreach header : ['xcb.h', 'xproto.h']
  configure_file(
    input : xcb_system_include_dir / 'xcb' / header,
    output : header,
    copy : true,
  )
endforeach
configure_file(
  input : 'xcb_windefs.h',
  output : 'xcb_windefs.h',
  copy : true,
)
