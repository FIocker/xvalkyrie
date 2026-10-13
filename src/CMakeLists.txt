# Shared by every target
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(formats)
add_subdirectory(compatibility)
add_subdirectory(loader)
add_subdirectory(graphics)
# runtime/common is shared, runtime/durango and runtime/scarlett are one console each
add_subdirectory(runtime)
# The one executable, which picks the console runtime per title
add_subdirectory(frontend)
add_subdirectory(app)
