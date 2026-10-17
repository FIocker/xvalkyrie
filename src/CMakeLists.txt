# Layers, lowest first. A directory only uses the ones above it in this list.
add_subdirectory(core)
add_subdirectory(host)
add_subdirectory(formats)
add_subdirectory(analysis)
add_subdirectory(loader)
# What a title's graphics, sound, input and window end up on: one directory per kind,
# one subdirectory per host technology
add_subdirectory(backends)
# runtime/common and runtime/modules are shared, runtime/durango and runtime/scarlett are
# one console each
add_subdirectory(runtime)
add_subdirectory(launcher)
# The one executable, which picks the console runtime per title
add_subdirectory(app)
add_subdirectory(tools)
