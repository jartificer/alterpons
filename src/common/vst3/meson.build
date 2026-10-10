# Meson does not allow mixing native and non native dependencies from
# subprojects. The only workaround is to only define only the necessary
# variables there, and to then assemble the dependencies here ourselves.

vst3 = subproject('vst3', version : '3.7.7')

# Also include our normal compiler options so symbols are hidden by default
vst3_compiler_options = compiler_options + vst3.get_variable('compiler_options')
vst3_include_dir = vst3.get_variable('include_dir')

# We need to do some minor hacking to get the SDK to compile with winegcc. Most
# notably some attributes are named differently, and the SDK uses 'Windows.h'
# instead of 'windows.h' like how the file is actually called.
# message(vst3_include_dir)
vst3_sdk_base_dir = vst3.get_variable('sdk_base_dir')
patch_result = run_command('../../../tools/patch-vst3-sdk.sh', vst3_sdk_base_dir, check : true)
message(patch_result.stdout())

if not is_pe_host_build
# We'll create a dependency for the plugin SDK for our native VST3 plugin
vst3_base_native = static_library(
  'base_native',
  vst3.get_variable('base_sources'),
  native : true,
  cpp_args : vst3_compiler_options + ['-Wno-cpp'],
  include_directories : vst3_include_dir,
  override_options : ['warning_level=0'],
)
vst3_pluginterfaces_native = static_library(
  'pluginterfaces_native',
  vst3.get_variable('pluginterfaces_sources'),
  native : true,
  cpp_args : vst3_compiler_options,
  include_directories : vst3_include_dir,
  override_options : ['warning_level=0'],
)
vst3_sdk_native = static_library(
  'sdk_native',
  vst3.get_variable('sdk_common_sources') + vst3.get_variable('sdk_sources'),
  native : true,
  link_with : [vst3_base_native, vst3_pluginterfaces_native],
  cpp_args : vst3_compiler_options + ['-Wno-multichar'],
  include_directories : vst3_include_dir,
  override_options : ['warning_level=0'],
)
endif

# We need a separate host SDK dependency for our Wine plugin host applications
if is_64bit_system and not is_pe_host_build
  vst3_base_wine_64bit = static_library(
    'vst3_base_wine_64bit',
    vst3.get_variable('base_sources'),
    native : false,
    cpp_args : vst3_compiler_options + wine_64bit_compiler_options + [ '-Wno-cpp'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_pluginterfaces_wine_64bit = static_library(
    'vst3_pluginterfaces_wine_64bit',
    vst3.get_variable('pluginterfaces_sources'),
    native : false,
    cpp_args : vst3_compiler_options + wine_64bit_compiler_options,
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_sdk_hosting_wine_64bit = static_library(
    'vst3_sdk_hosting_wine_64bit',
    vst3.get_variable('sdk_common_sources') + vst3.get_variable('sdk_hosting_sources'),
    native : false,
    link_with : [vst3_base_wine_64bit, vst3_pluginterfaces_wine_64bit],
    cpp_args : vst3_compiler_options + wine_64bit_compiler_options + ['-Wno-multichar'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
endif

# And another time for the 32-bit PE host. The SDK defines `UNICODE` in
# `ftypes.h`, but we force `winsock2.h` (and thus `windows.h`) to be included
# first in the PE host so we need to define it up front.
if is_pe_host_build
  vst3_pe_compiler_options = pe_host_compiler_options + ['-DUNICODE', '-D_UNICODE']
  vst3_base_wine_32bit = static_library(
    'vst3_base_wine_32bit',
    vst3.get_variable('base_sources'),
    native : false,
    cpp_args : vst3_compiler_options + vst3_pe_compiler_options + ['-Wno-cpp'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_pluginterfaces_wine_32bit = static_library(
    'vst3_pluginterfaces_wine_32bit',
    vst3.get_variable('pluginterfaces_sources'),
    native : false,
    cpp_args : vst3_compiler_options + vst3_pe_compiler_options,
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_sdk_hosting_wine_32bit = static_library(
    'vst3_sdk_hosting_wine_32bit',
    vst3.get_variable('sdk_common_sources') + vst3.get_variable('sdk_hosting_sources'),
    native : false,
    link_with : [vst3_base_wine_32bit, vst3_pluginterfaces_wine_32bit],
    cpp_args : vst3_compiler_options + vst3_pe_compiler_options + ['-Wno-multichar'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
endif

#
# VST3 dependencies
#

if not is_pe_host_build
  vst3_sdk_native_dep = declare_dependency(
    link_with : vst3_sdk_native,
    include_directories : vst3_include_dir,
    compile_args : vst3_compiler_options,
  )
endif

if is_64bit_system and not is_pe_host_build
  vst3_sdk_hosting_wine_64bit_dep = declare_dependency(
    link_with : vst3_sdk_hosting_wine_64bit,
    include_directories : vst3_include_dir,
    # This does mean that we now have a lot of defines in our code, but the
    # alternative would be patching every location in the SDK where they include
    # `windows.h`
    compile_args : vst3_compiler_options + wine_64bit_compiler_options,
  )
endif

if is_pe_host_build
  vst3_sdk_hosting_wine_32bit_dep = declare_dependency(
    link_with : vst3_sdk_hosting_wine_32bit,
    include_directories : vst3_include_dir,
    # This does mean that we now have a lot of defines in our code, but the
    # alternative would be patching every location in the SDK where they include
    # `windows.h`
    compile_args : vst3_compiler_options + pe_host_compiler_options,
  )
endif
