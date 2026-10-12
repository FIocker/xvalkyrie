# Small Windows programs that the runtime loads in the guest_execution test.
string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
# only built for its import lib, so the guest has an import nobody implements
add_library(xs_guest_absent SHARED absent.c)
target_link_options(xs_guest_absent PRIVATE /NODEFAULTLIB /NOENTRY)
add_executable(xs_guest_raw raw.c)
target_compile_options(xs_guest_raw PRIVATE /GS-)
target_link_options(xs_guest_raw PRIVATE /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:CONSOLE /INCREMENTAL:NO)
target_link_libraries(xs_guest_raw PRIVATE xs_guest_absent kernel32)
add_executable(xs_guest_crt crt.cpp)
target_link_options(xs_guest_crt PRIVATE /INCREMENTAL:NO)
set_target_properties(xs_guest_absent xs_guest_raw xs_guest_crt PROPERTIES
  MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
