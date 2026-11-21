# Gamepad input in an Unreal Engine 4 title

How one Unreal Engine 4 title (Xbox One, XDK era) finds its gamepads, from
its call sites. The code is in `src/runtime/durango/winrt/gamepad.cpp`,
`placeholder.cpp` and `inert.cpp`.

## What the title does

1. Asks `Windows.Xbox.Input.Controller` for its statics
   (`7400624f-144b-47fc-9f3b-1dcafb482873`) and reads `Controllers`
   (method 6). It also registers for `ControllerAdded`, `ControllerRemoved`
   and two more events.
2. Uses each item of the list as an `IController`
   (`9776ff56-9bfd-4581-ad45-b645bba526d6`): `Id` (6), `Type` (7), `User` (8).
3. Queries each controller for `c87ec7f7-d0b8-43ac-8a2f-e1983179402c`.
   A controller that answers is left out. Every place that walks the
   controllers does this first.
4. Queries the controllers it kept for `IGamepad`
   (`082e402c-07df-45e1-a5ab-a3127af197b5`) and calls
   `GetRawCurrentReading` (method 8) on it every frame.
5. On the first button, calls `IUserStatics::GetUserById` (method 14 of
   `b4f23a93-7fc6-4a50-b153-950016389e50`) with the controller's user id and
   throws if it fails.

## What went wrong

A placeholder answers any `QueryInterface` with a facet of itself. That is
right for interfaces nobody has looked at, and wrong in two cases here:

- The interface of step 3 is asked in order to exclude. Saying yes dropped
  the one gamepad, so the title never read it. It gets `E_NOINTERFACE`.
- `IGamepad` asked of the controller facet has to be the gamepad itself, not
  another placeholder.

`GetUserById` was unimplemented. The exception it caused left the game thread
waiting for the render thread until Unreal's 30 second timeout.

## Checking

The log says `the title reads the gamepad` on the first reading and
`first gamepad button reached the title` on the first button.
`XV_INPUT_PRESS=a@40` holds A for half a second, 40 seconds after the first
reading, which is enough to get past a title screen without a person.
