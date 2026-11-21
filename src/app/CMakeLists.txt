# Two executables, side by side. xvalkyrie is the window. xvboot runs a title, in a process
# and a window of its own, and has the commands that look at one; the window starts it for
# each title.
add_subdirectory(cli)
add_subdirectory(bootstrap)
add_subdirectory(gui)
